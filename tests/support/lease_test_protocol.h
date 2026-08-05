#pragma once

namespace cha::test {

inline constexpr int lease_probe_acquired = 0;
inline constexpr int lease_probe_busy = 10;
inline constexpr int lease_probe_failed = 11;
inline constexpr int catalog_create_succeeded = 20;
inline constexpr int catalog_create_exists = 21;
inline constexpr int catalog_create_busy = 22;
inline constexpr char lease_holder_ready = '1';

} // namespace cha::test
