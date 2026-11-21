# tools/

## provision_nvs.ps1

Writes the network configuration straight into the NVS partition over USB,
without going through the on-screen Settings page. Handy to provision several
boards, or a board whose screen is not reachable.

```powershell
.\tools\provision_nvs.ps1 -Ssid "my-wifi" -Pass "wifi-password" -HostAddr "192.168.1.10"
```

- `-Port COMx` to use another port than `COM7`.
- `-DryRun` builds the NVS image (`nvs.bin`, git-ignored) without flashing it.
- The board reboots on the new configuration once flashed.

> ⚠️ **The script replaces the whole NVS partition.** Every field you leave out
> falls back to the compiled defaults (`main/app_secrets.h` if present, empty
> otherwise), and the comfort preferences (volume, brightness, ringtone…) are
> reset. It also erases the **API encryption key** Home Assistant provisioned:
> HA then reports an invalid key until you delete the device from the ESPHome
> integration and adopt it again. To change a single value, the Settings page
> on the touch screen is the safer path.

The CSV holding your Wi-Fi password is generated in `%TEMP%` and deleted on
exit; it never lands in the repository.

The script assumes the default Windows install of the ESP-IDF VS Code
extension (`C:\esp\v6.0.1\esp-idf`, `C:\Espressif\tools`). Adjust the paths at
the top of the script for another layout.
