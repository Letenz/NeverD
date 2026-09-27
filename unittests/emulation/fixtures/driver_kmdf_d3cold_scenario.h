//===- driver_kmdf_d3cold_scenario.h - Cold power scenario contract ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share explicit cold-power scenario inputs between native and public tests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DRIVER_KMDF_D3COLD_SCENARIO_H
#define NEVERD_DRIVER_KMDF_D3COLD_SCENARIO_H

#include "driver_kmdf_power_policy_test.h"

#include <string>

namespace neverd::test {
inline std::string kmdfD3ColdScenario(char Mode, bool DefaultEnabled,
                                      bool WakeFromCold,
                                      bool Supported = true) {
  const auto Boolean = [](bool Value) { return Value ? "true" : "false"; };
  return std::string(R"({"service_name":"NeverDKmdfPower)") + Mode +
         R"(-","unload":true,"pnp_devices":[{
    "id":"cold-pdo","bus":"register_bank","initial_device_power":"D0",
    "initial_system_power":"working","wake_capabilities":{"s0":true,"sx":true},
    "resources":[{"id":"bar0","raw_start":"0x200000000",
      "translated_start":"0x300000000","length":4096,
      "registers":[{"offset":0,"width":4,"access":"read_write",
                    "value":"0x12345678"}]}],
    "d3cold":{"supported":)" +
         Boolean(Supported) + R"(,"enabled_by_default":)" +
         Boolean(DefaultEnabled) + R"(,"wake_s0":)" + Boolean(WakeFromCold) +
         R"(,"wake_sx":false},"requested_device_power":[
      {"minor":"set","power_type":"device","power_state":"D3",
       "power_action":"none","system_context":0,
       "bus_completion":{"status":0,"delay_100ns":3}},
      {"minor":"set","power_type":"device","power_state":"D0",
       "power_action":"none","system_context":0,
       "bus_completion":{"status":0,"delay_100ns":3}}]}],
    "requests":[
      {"kind":"pnp","device_id":"cold-pdo","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"create","device_id":"cold-pdo","file":1},
      {"kind":"ioctl","device_id":"cold-pdo","file":1,"code":"0x222000",
       "input":"00","output_size":32,"power_policy_events":[
         {"device_id":"cold-pdo","after_100ns":0,"action":"idle"},
         {"device_id":"cold-pdo","after_100ns":10017,"action":")" +
         (Mode == KmdfPowerColdNoWake ? "active" : "wake") +
         R"("}]},
      {"kind":"ioctl","device_id":"cold-pdo","file":1,"code":"0x222000",
       "input":"00","output_size":32},
      {"kind":"cleanup","device_id":"cold-pdo","file":1},
      {"kind":"close","device_id":"cold-pdo","file":1},
      {"kind":"pnp","device_id":"cold-pdo","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"cold-pdo","minor":"remove",
       "bus_completion":{"status":0}}]})";
}
} // namespace neverd::test
#endif // NEVERD_DRIVER_KMDF_D3COLD_SCENARIO_H
