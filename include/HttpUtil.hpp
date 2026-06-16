#pragma once

#include <string>
#include <boost/beast/http/verb.hpp>

namespace Exchange {

std::string perform_https_request(
    const std::string& host,
    const std::string& port,
    boost::beast::http::verb method,
    const std::string& target
);

} // namespace Exchange
