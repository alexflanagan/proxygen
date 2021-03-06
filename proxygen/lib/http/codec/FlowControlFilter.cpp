/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/FlowControlFilter.h>

#include <proxygen/lib/http/codec/SPDYConstants.h>

namespace proxygen {

namespace {
HTTPException getException() {
  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS);
  ex.setCodecStatusCode(ErrorCode::FLOW_CONTROL_ERROR);
  return ex;
}

}

FlowControlFilter::FlowControlFilter(Callback& callback,
                                     folly::IOBufQueue& writeBuf,
                                     HTTPCodec* codec,
                                     uint32_t recvCapacity):
    notify_(callback),
    recvWindow_(spdy::kInitialWindow),
    sendWindow_(spdy::kInitialWindow),
    error_(false),
    sendsBlocked_(false) {
  if (recvCapacity < spdy::kInitialWindow) {
    VLOG(4) << "Ignoring low conn-level recv window size of " << recvCapacity;
  } else if (recvCapacity > spdy::kInitialWindow) {
    auto delta = recvCapacity - spdy::kInitialWindow;
    VLOG(4) << "Incrementing default conn-level recv window by " << delta;
    CHECK(recvWindow_.setCapacity(recvCapacity));
    codec->generateWindowUpdate(writeBuf, 0, delta);
  }
}

void FlowControlFilter::setReceiveWindowSize(folly::IOBufQueue& writeBuf,
                                             uint32_t capacity) {
  if (capacity < spdy::kInitialWindow) {
    VLOG(4) << "Ignoring low conn-level recv window size of " << capacity;
    return;
  }
  int32_t delta = capacity - recvWindow_.getCapacity();
  if (delta < 0) {
    // For now, we're disallowing shrinking the window, since it can lead
    // to FLOW_CONTROL_ERRORs if there is data in flight.
    VLOG(4) << "Refusing to shrink the recv window";
    return;
  }
  VLOG(4) << "Incrementing default conn-level recv window by " << delta;
  if (!recvWindow_.setCapacity(capacity)) {
    VLOG(2) << "Failed setting conn-level recv window capacity to " << capacity;
    return;
  }
  toAck_ += delta;
  if (toAck_ > 0) {
    call_->generateWindowUpdate(writeBuf, 0, delta);
    toAck_ = 0;
  }
}

bool FlowControlFilter::ingressBytesProcessed(folly::IOBufQueue& writeBuf,
                                              uint32_t delta) {
  toAck_ += delta;
  if (toAck_ > 0 && uint32_t(toAck_) > recvWindow_.getCapacity() / 2) {
    CHECK(recvWindow_.free(toAck_));
    call_->generateWindowUpdate(writeBuf, 0, toAck_);
    toAck_ = 0;
    return true;
  }
  return false;
}

uint32_t FlowControlFilter::getAvailableSend() const {
  return sendWindow_.getNonNegativeSize();
}

bool FlowControlFilter::isReusable() const {
  if (error_) {
    return false;
  }
  return call_->isReusable();
}

void FlowControlFilter::onBody(StreamID stream,
                               std::unique_ptr<folly::IOBuf> chain) {
  if (!recvWindow_.reserve(chain->computeChainDataLength())) {
    error_ = true;
    HTTPException ex = getException();
    callback_->onError(0, ex, false);
  } else {
    callback_->onBody(stream, std::move(chain));
  }
}

void FlowControlFilter::onWindowUpdate(StreamID stream, uint32_t amount) {
  if (!stream) {
    bool success = sendWindow_.free(amount);
    if (!success) {
      LOG(WARNING) << "Remote side sent connection-level WINDOW_UPDATE "
                   << "that could not be applied. Aborting session.";
      // If something went wrong applying the flow control change, abort
      // the entire session.
      error_ = true;
      HTTPException ex = getException();
      callback_->onError(stream, ex, false);
    }
    if (sendsBlocked_ && sendWindow_.getNonNegativeSize()) {
      sendsBlocked_ = false;
      notify_.onConnectionSendWindowOpen();
    }
    // Don't forward.
  } else {
    callback_->onWindowUpdate(stream, amount);
  }
}

size_t FlowControlFilter::generateBody(folly::IOBufQueue& writeBuf,
                                       StreamID stream,
                                       std::unique_ptr<folly::IOBuf> chain,
                                       bool eom) {
  bool success = sendWindow_.reserve(chain->computeChainDataLength());
  // In the future, maybe make this DCHECK
  CHECK(success) << "Session-level send window underflowed! "
                 << "Too much data sent without WINDOW_UPDATES!";

  if (sendWindow_.getNonNegativeSize() == 0) {
    // Need to inform when the send window is no longer full
    sendsBlocked_ = true;
  }

  return call_->generateBody(writeBuf, stream, std::move(chain), eom);
}

size_t FlowControlFilter::generateWindowUpdate(folly::IOBufQueue& writeBuf,
                                               StreamID stream,
                                               uint32_t delta) {
  CHECK(stream) << " someone tried to manually manipulate a conn-level window";
  return call_->generateWindowUpdate(writeBuf, stream, delta);
}

}
