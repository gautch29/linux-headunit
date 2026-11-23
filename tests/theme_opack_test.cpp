// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/opack.hpp>
#include <iostream>
#include <stdexcept>
using namespace headunit::theme;
namespace op = headunit::theme::opack;
void require(bool v) {
  if (!v) throw std::runtime_error("OPACK test failed");
}
void rejects(const Bytes& b) {
  try {
    (void)op::decode(b);
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("OPACK accepted malformed input");
}
int main() {
  try {
    // Three fixtures produced from our synthetic dictionaries by the installed
    // Xcode SDK, plus malformed and resource-bound cases.
    op::Value dict{
        op::Dictionary{{op::Value{std::string("command")}, op::Value{std::string("configRequest")}},
                       {op::Value{std::string("transactionID")}, op::Value{std::int64_t(1)}}}};
    require(op::decode(op::encode(dict)) == dict);
    Bytes repeated{0xe1, 0x46, 'v',  'a', 'l',  'u', 'e', 's', 0xd7, 9,   2,
                   7,    0x31, 0,    1,   0x36, 0,   0,   0,   0,    0,   0,
                   0x0c, 0x40, 0x46, 'r', 'e',  'p', 'e', 'a', 't',  0xa3};
    auto parsed = op::decode(repeated);
    auto& values = std::get<op::Array>(std::get<op::Dictionary>(parsed.data)[0].second.data);
    require(values.size() == 7 && values[5] == values[6]);
    require(std::get<double>(values[4].data) == 3.5);
    require(op::decode(op::encode(parsed)) == parsed);
    for (const Bytes& b : {Bytes{}, Bytes{3}, Bytes{0xa0}, Bytes{0xc1, 0}, Bytes{0xd1},
                           Bytes{0x61, 255}, Bytes{0xe1, 0x41, 'k'}, Bytes{0xdf, 8}, Bytes{8, 9},
                           Bytes{0x42, 0xc0, 0x80}, Bytes{0xe2, 0x41, 'k', 8, 0x41, 'k', 9}})
      rejects(b);
    Bytes deep(34, 0xd1);
    deep.push_back(8);
    rejects(deep);
    Bytes wide(16386, 8);
    wide[0] = 0xdf;
    wide.push_back(3);
    rejects(wide);
    Bytes bomb{0xdf, 0x92, 0, 0x80};
    bomb.resize(4 + 32768, 'x');
    bomb.insert(bomb.end(), 200, 0xa0);
    bomb.push_back(3);
    rejects(bomb);
    // Every truncation of a complete nested message is rejected.
    for (std::size_t i = 0; i < repeated.size(); ++i)
      rejects(Bytes(repeated.begin(), repeated.begin() + i));
    op::Array many(50);
    for (std::size_t i = 0; i < many.size(); ++i)
      many[i].data = std::int64_t(i);
    op::Value indefinite{many};
    require(op::decode(op::encode(indefinite)) == indefinite);
    op::Value keys{op::Dictionary{{op::Value{std::int64_t(17)}, op::Value{true}}}};
    require(op::decode(op::encode(keys)) == keys);
    // SDK integer payloads are signed at their encoded width.
    require(std::get<std::int64_t>(op::decode(Bytes{0x30, 0xfe}).data) == -2);
    require(std::get<std::int64_t>(op::decode(Bytes{0x31, 0, 0x80}).data) == -32768);
    require(op::encode(op::Value{std::int64_t(32768)}) == Bytes({0x32, 0, 0x80, 0, 0}));
    // SDK's long string lengths use 1, 2, 4, 8 byte widths, not 1, 2, 3, 4.
    op::Value long_string{std::string(65536, 's')};
    auto long_wire = op::encode(long_string);
    require(long_wire[0] == 0x63 && long_wire[3] == 1 && long_wire[4] == 0);
    require(op::decode(long_wire) == long_string);
    std::cout << "OPACK codec: fixtures, references, boundaries and malformed inputs passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
