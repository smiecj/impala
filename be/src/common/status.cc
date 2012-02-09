// Copyright (c) 2011 Cloudera, Inc. All rights reserved.

#include <boost/algorithm/string/join.hpp>

#include "common/status.h"

using namespace std;
using namespace boost::algorithm;

namespace impala {

struct Status::ErrorDetail {
  vector<string> error_msgs;

  ErrorDetail(const string& msg): error_msgs(1, msg) {}
  ErrorDetail(const vector<string>& msgs): error_msgs(msgs) {}
};

const Status Status::OK;

Status::Status(const string& error_msg)
  : error_detail_(new ErrorDetail(error_msg)) {
}

Status::Status(const Status& status)
  : error_detail_(
      status.error_detail_ != NULL
        ? new ErrorDetail(*status.error_detail_)
        : NULL) {
}

Status& Status::operator=(const Status& status) {
  delete error_detail_;
  if (status.error_detail_ == NULL) {
    error_detail_ = NULL;
  } else {
    error_detail_ = new ErrorDetail(*status.error_detail_);
  }
  return *this;
}

Status::Status(const TStatus& status)
  : error_detail_(
      status.error_msgs.empty() ? NULL : new ErrorDetail(status.error_msgs)) {
}

Status& Status::operator=(const TStatus& status) {
  delete error_detail_;
  if (status.status_code == 0) {
    error_detail_ = NULL;
  } else {
    error_detail_ = new ErrorDetail(status.error_msgs);
  }
  return *this;
}

Status::~Status() {
  if (error_detail_ != NULL) delete error_detail_;
}

void Status::GetErrorMsgs(vector<string>* msgs) const {
  msgs->clear();
  if (error_detail_ != NULL) {
    *msgs = error_detail_->error_msgs;
  }
}

void Status::GetErrorMsg(string* msg) const {
  msg->clear();
  if (error_detail_ != NULL) {
    *msg = join(error_detail_->error_msgs, "\n");
  }
}

string Status::GetErrorMsg() const {
  string msg;
  GetErrorMsg(&msg);
  return msg;
}

void Status::ToThrift(TStatus* status) {
  status->error_msgs.clear();
  if (error_detail_ == NULL) {
    status->status_code = TStatusCode::OK;
  } else {
    // TODO: add a TStatusCode Status::ErrorDetail::status_code
    status->status_code = TStatusCode::INTERNAL_ERROR;
    for (int i = 0; i < error_detail_->error_msgs.size(); ++i) {
      status->error_msgs.push_back(error_detail_->error_msgs[i]);
    }
  }
}

}
