# Third-party components

This project includes the following third-party code. Each keeps its own licence;
the full licence texts are in the folders listed.

| Component | Version | Licence | Location |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0 | MIT | `Lift Axle Extension/imgui/LICENSE.txt` |
| [MinHook](https://github.com/TsudaKageyu/minhook) | 1.3.4 | BSD 2-Clause | `Lift Axle Extension/minhook/LICENSE.txt` |
| SCS SDK (telemetry headers) | 1.16 | MIT-style, © SCS Software | `Lift Axle Extension/scs_sdk/LICENSE.txt` |

Only the files needed to build the plugin are included (ImGui core + DirectX 11 backend,
MinHook sources, SDK headers). The released DLL contains ImGui and MinHook compiled in;
their licence notices apply to it as well.
