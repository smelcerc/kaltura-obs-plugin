#pragma once

#include "kaltura_live/api/http_transport.hpp"

#include <QTimer>

#include <deque>
#include <utility>
#include <vector>

namespace kaltura_live::api::test {

class MockHttpTransport final : public HttpTransport {
public:
  void enqueue(HttpResponse response) { responses_.push_back(std::move(response)); }

  void post(HttpRequest request, HttpCompletion completion) override
  {
    requests_.push_back(std::move(request));
    HttpResponse response;
    if (responses_.empty()) {
      response.errorMessage = "Mock response queue is empty";
    } else {
      response = std::move(responses_.front());
      responses_.pop_front();
    }
    QTimer::singleShot(0, [completion = std::move(completion),
                           response = std::move(response)]() mutable {
      completion(std::move(response));
    });
  }

  [[nodiscard]] const std::vector<HttpRequest> &requests() const { return requests_; }

private:
  std::deque<HttpResponse> responses_;
  std::vector<HttpRequest> requests_;
};

}  // namespace kaltura_live::api::test
