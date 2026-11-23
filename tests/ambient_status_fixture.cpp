// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Portable actual software owner. Stdio is an explicit test transport only.
#include <headunit/ambient_light_publication.hpp>
#include <iostream>

int main() try {
  using namespace headunit;
  ambient::SoftwareNonce nonce{};
  nonce.fill(std::byte{'n'});
  ambient::SoftwareModel model;
  std::uint64_t sequence = 0;
  const auto publish = [&] {
    const auto packet = model.publish({}, nonce, ++sequence, Clock::now(), std::nullopt);
    std::cout.write(reinterpret_cast<const char*>(packet.data()), packet.size());
    std::cout.flush();
  };
  publish();
  ambient::SoftwareAction action{};
  while (std::cin.read(reinterpret_cast<char*>(action.data()), action.size())) {
    if (!model.change(action, nonce, Clock::now())) break;
    publish();
  }
  model.retire(Clock::now());
  try {
    publish();
    return 2;  // A retired original owner cannot publish a successor model.
  } catch (const std::invalid_argument&) {
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
