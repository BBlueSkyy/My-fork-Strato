# Auditoria de settings / set:sys

Base da PR: `92f2d083f32cda62c9f4236debf72785f1980491` (`master`).
Branch: `fix/settings-set-sys-modernization`.
Os sete blocos de settings foram transportados para essa base sem incluir as alterações de AM da base original; o checkout original foi preservado.

## Referências e critério

- [Eden](https://git.eden-emu.dev/eden-emu/eden/src/commit/858f9e5aeac89503c331dfe4588cc3c83ab2223b/src/core/hle/service/set), revisão `858f9e5aeac89503c331dfe4588cc3c83ab2223b`.
- [Ryujinx](https://github.com/alula/Ryujinx/tree/1df6c07f78c4c3b8c7fc679d7466f79a10c2d496/src/Ryujinx.HLE/HOS/Services/Settings), revisão `1df6c07f78c4c3b8c7fc679d7466f79a10c2d496` (espelho alula).
- [Switchbrew: Settings services](https://switchbrew.org/wiki/Settings_services), consulta de 2026-09-09: IDs e introdução por firmware. S1/S2 são as plataformas indicadas pela fonte; comandos exclusivos de S2 não são registrados.
- [libnx set.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/set.c) e [set.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/set.h): argumentos inline, tipos de buffer e estruturas.
- [Atmosphère: resultados de settings](https://github.com/Atmosphere-NX/Atmosphere/blob/cb4b882/libraries/libvapours/include/vapours/results/settings_results.hpp): módulo 105, item ausente 11, nomes nulos/vazios/longos.

“Handler” descreve apenas a presença de uma função na referência. Não significa implementação completa: Ryujinx tem stubs e caminhos de settings-item que retornam Success sem valor; Eden também tem stubs e funções ausentes. Esses comportamentos não foram copiados.

## Comportamento e limites

- O dispatcher exclusivo de settings retorna CMIF `10/221` para comando ausente/TIPC. O dispatcher global e os outros serviços permanecem intactos.
- GetFirmwareVersion2 preserva os bytes do perfil HLE; GetFirmwareVersion limpa somente revision_minor. O perfil 9.0.0 existente permanece: este fork não tem importador de SystemVersion. Não se declara suporte integral a firmware 18/20/22 apenas por implementar seus IDs.
- Consultas de idioma preservam o idioma de sistema (inclusive pt-BR), em vez de convertê-lo para idioma de aplicação. Listas antigas têm 15 entradas; modernas retornam as 18 realmente disponíveis no fork. Eden oferece idiomas extras em sua revisão, mas não adicionamos idiomas sem suporte no frontend.
- GetSettingsItemValueSize/Value compartilham um catálogo tipado explícito. Desconhecidos retornam SettingsItemNotFound (105/11). Buffers curtos recebem min(tamanho do valor, tamanho do buffer) e o u64 real copiado. Não há zero genérico para chave desconhecida. O ano inicial 2019 vem do perfil da referência Ryujinx; os parâmetros de relógio existentes concordam com timesrv (30 dias de precisão e zero test offset). O catálogo não substitui a base inteira de configurações do firmware.
- Preferências implementadas são gravadas em `privateAppFilesPath/system-settings.bin`, formato próprio versionado. Save usa arquivo temporário, flush, fsync e rename; falha não altera o valor em memória. Arquivo inválido é preservado e o acesso retorna erro. Idioma/região reaplicam o estado compartilhado no próximo início.
- Clock source/context, offset, reset, correção automática e timezone são lidos do TimeServiceObject existente. Setters suportados atualizam o core/callbacks existentes; não se restaura UUID de um boot anterior. Esses ajustes de tempo têm o ciclo de vida atual de timesrv, por execução. SetExternalSteadyClockSourceId, SetExternalRtcResetFlag, SetExternalSteadyClockInternalOffset e SetUserSystemClockAutomaticCorrectionUpdatedTime retornam NotImplemented porque as APIs atuais não permitem atualização consistente sem ampliar o escopo. Timezone usa somente nomes presentes na lista de tzdata e atualiza o nome com padding completo. O evento de mudança de localização da camada glue não é ampliado por esta branch.
- KeyboardLayout persiste; os mapas reais de 0x1000 bytes de Ryujinx foram preservados com licença MIT. O fallback EnglishUs segue Eden (US internacional); GetKeyCodeMap v1 mantém a diferença do mapa japonês documentada por Ryujinx. Todos os ports virtuais usam o mesmo layout, como Eden.
- PlatformRegion (1 Global / 2 Terra) é distinto de RegionCode (0–5). GetT/SetT são aliases do primeiro. TouchScreenMode guarda Standard/Stylus; não altera sensibilidade física da tela Android.
- WirelessLanEnableFlag acompanha a configuração de rede usada por nifm. O guest pode desabilitar/reabilitar a rede somente se o usuário já a permitia ao iniciar. Bluetooth/NFC/USB guest não têm backend de rádio/transporte: seus getters informam desabilitado e habilitar retorna erro; desabilitar um transporte já desligado é idempotente. Não se inventa sucesso de conexão. As limitações preexistentes de nifm (como o status de conexão fixo) não são corrigidas aqui.
- Preferências de lock screen, telemetria/download automático, kiosk, atualizações, bateria, notificações, idioma/entrada, EULA e sleep são registros de configuração. Salvar uma preferência não executa uma atualização/download ou aceita EULAs automaticamente. A lista inicial de EULA é vazia.
- Não são implementados certificados, calibração, chaves, firmware digest, recursos S2 e demais linhas marcadas como erro. A branch moderniza os blocos descritos; **não é equivalência completa de set:sys ao hardware/Eden**.

## set — todos os IDs documentados

| ID | Firmware / comando | Eden | Ryujinx | Branch |
|---:|---|---|---|---|
| 0 | GetLanguageCode | handler | handler | implementado |
| 1 | GetAvailableLanguageCodes | handler | handler | implementado |
| 2 | [4.0.0+] MakeLanguageCode | handler | handler | implementado |
| 3 | GetAvailableLanguageCodeCount | handler | handler | implementado |
| 4 | GetRegionCode | handler | handler | implementado |
| 5 | [4.0.0+] GetAvailableLanguageCodes2 | handler | handler | implementado |
| 6 | [4.0.0+] GetAvailableLanguageCodeCount2 | handler | handler | implementado |
| 7 | [4.0.0+] GetKeyCodeMap | handler | handler | implementado |
| 8 | [5.0.0+] GetQuestFlag | handler | handler | implementado |
| 9 | [6.0.0+] GetKeyCodeMap2 | handler | handler | implementado |
| 10 | [9.0.0+] GetFirmwareVersionForDebug | ausente | ausente | erro; perfil retail |
| 11 | [10.1.0+] GetDeviceNickName | handler | handler | implementado |
| 12 | [18.0.0+] GetKeyCodeMapByPort | handler | ausente | implementado |

## set:sys — todos os IDs documentados

| ID | Firmware / comando (Switchbrew) | Eden | Ryujinx | Branch |
|---:|---|---|---|---|
| 0 | SetLanguageCode | handler | ausente | implementado |
| 1 | SetNetworkSettings | ausente | ausente | erro CMIF; sem implementação |
| 2 | GetNetworkSettings | ausente | ausente | erro CMIF; sem implementação |
| 3 | [S1] GetFirmwareVersion | handler | handler | perfil HLE 9.0.0; buffer validado |
| 4 | [3.0.0+] GetFirmwareVersion2 | handler | handler | perfil HLE 9.0.0; buffer validado |
| 5 | [S1] [5.0.0+] GetFirmwareVersionDigest | ausente | ausente | erro CMIF; sem implementação |
| 7 | GetLockScreenFlag | handler | ausente | implementado |
| 8 | SetLockScreenFlag | handler | ausente | implementado |
| 9 | GetBacklightSettings | ausente | ausente | erro CMIF; sem implementação |
| 10 | SetBacklightSettings | ausente | ausente | erro CMIF; sem implementação |
| 11 | [S1] SetBluetoothDevicesSettings | ausente | ausente | erro CMIF; sem implementação |
| 12 | [S1] GetBluetoothDevicesSettings | ausente | ausente | erro CMIF; sem implementação |
| 13 | GetExternalSteadyClockSourceId | handler | ausente | estado vivo de timesrv; por execução |
| 14 | SetExternalSteadyClockSourceId | handler | ausente | erro; API de timesrv insuficiente |
| 15 | GetUserSystemClockContext | handler | ausente | estado vivo de timesrv; por execução |
| 16 | SetUserSystemClockContext | handler | ausente | estado vivo de timesrv; por execução |
| 17 | GetAccountSettings | handler | ausente | implementado |
| 18 | SetAccountSettings | handler | ausente | implementado |
| 19 | GetAudioVolume | ausente | ausente | erro CMIF; sem implementação |
| 20 | SetAudioVolume | ausente | ausente | erro CMIF; sem implementação |
| 21 | GetEulaVersions | handler | ausente | implementado |
| 22 | SetEulaVersions | handler | ausente | implementado |
| 23 | GetColorSetId | handler | handler | implementado |
| 24 | SetColorSetId | handler | handler | implementado |
| 25 | [S1] GetConsoleInformationUploadFlag | handler | ausente | implementado |
| 26 | [S1] SetConsoleInformationUploadFlag | handler | ausente | implementado |
| 27 | [S1] GetAutomaticApplicationDownloadFlag | handler | ausente | implementado |
| 28 | [S1] SetAutomaticApplicationDownloadFlag | handler | ausente | implementado |
| 29 | GetNotificationSettings | handler | ausente | implementado |
| 30 | SetNotificationSettings | handler | ausente | implementado |
| 31 | GetAccountNotificationSettings | handler | ausente | implementado |
| 32 | SetAccountNotificationSettings | handler | ausente | implementado |
| 35 | GetVibrationMasterVolume | handler | ausente | erro CMIF; sem implementação |
| 36 | SetVibrationMasterVolume | handler | ausente | erro CMIF; sem implementação |
| 37 | GetSettingsItemValueSize | handler | handler | implementado |
| 38 | GetSettingsItemValue | handler | handler | implementado |
| 39 | GetTvSettings | handler | ausente | erro CMIF; sem implementação |
| 40 | SetTvSettings | handler | ausente | erro CMIF; sem implementação |
| 41 | GetEdid | ausente | ausente | erro CMIF; sem implementação |
| 42 | SetEdid | ausente | ausente | erro CMIF; sem implementação |
| 43 | GetAudioOutputMode | handler | ausente | erro CMIF; sem implementação |
| 44 | SetAudioOutputMode | handler | ausente | erro CMIF; sem implementação |
| 45 | GetSpeakerAutoMuteFlag ([1.0.0-12.1.0] IsForceMuteOnHeadphoneRemoved) | handler | ausente | erro CMIF; sem implementação |
| 46 | SetSpeakerAutoMuteFlag ([1.0.0-12.1.0] SetForceMuteOnHeadphoneRemoved) | handler | ausente | erro CMIF; sem implementação |
| 47 | GetQuestFlag | handler | ausente | implementado |
| 48 | SetQuestFlag | handler | ausente | implementado |
| 49 | [S1] GetDataDeletionSettings | ausente | ausente | erro CMIF; sem implementação |
| 50 | [S1] SetDataDeletionSettings | ausente | ausente | erro CMIF; sem implementação |
| 51 | [S1] GetInitialSystemAppletProgramId | ausente | ausente | erro CMIF; sem implementação |
| 52 | [S1] GetOverlayDispProgramId | ausente | ausente | erro CMIF; sem implementação |
| 53 | GetDeviceTimeZoneLocationName | handler | ausente | estado vivo de timesrv; por execução |
| 54 | SetDeviceTimeZoneLocationName | handler | ausente | estado vivo de timesrv; por execução |
| 55 | [S1] GetWirelessCertificationFileSize | ausente | ausente | erro CMIF; sem implementação |
| 56 | [S1] GetWirelessCertificationFile | ausente | ausente | erro CMIF; sem implementação |
| 57 | SetRegionCode | handler | ausente | implementado |
| 58 | GetNetworkSystemClockContext | handler | ausente | estado vivo de timesrv; por execução |
| 59 | SetNetworkSystemClockContext | handler | ausente | estado vivo de timesrv; por execução |
| 60 | IsUserSystemClockAutomaticCorrectionEnabled | handler | handler | estado vivo de timesrv; por execução |
| 61 | SetUserSystemClockAutomaticCorrectionEnabled | handler | ausente | estado vivo de timesrv; por execução |
| 62 | GetDebugModeFlag | handler | handler | implementado |
| 63 | GetPrimaryAlbumStorage | handler | ausente | erro CMIF; sem implementação |
| 64 | SetPrimaryAlbumStorage | handler | ausente | erro CMIF; sem implementação |
| 65 | [S1] GetUsb30EnableFlag | handler | ausente | implementado |
| 66 | [S1] SetUsb30EnableFlag | handler | ausente | desabilitado; ativação retorna erro |
| 67 | GetBatteryLot | handler | ausente | erro CMIF; sem implementação |
| 68 | GetSerialNumber | handler | ausente | erro CMIF; sem implementação |
| 69 | GetNfcEnableFlag | handler | ausente | implementado |
| 70 | SetNfcEnableFlag | handler | ausente | desabilitado; ativação retorna erro |
| 71 | GetSleepSettings | handler | ausente | implementado |
| 72 | SetSleepSettings | handler | ausente | implementado |
| 73 | GetWirelessLanEnableFlag | handler | ausente | implementado |
| 74 | SetWirelessLanEnableFlag | handler | ausente | implementado |
| 75 | [S1] GetInitialLaunchSettings | handler | ausente | erro CMIF; sem implementação |
| 76 | [S1] SetInitialLaunchSettings | handler | ausente | erro CMIF; sem implementação |
| 77 | GetDeviceNickName | handler | handler | implementado |
| 78 | SetDeviceNickName | handler | handler | implementado |
| 79 | GetProductModel | handler | ausente | implementado |
| 80 | [S1] GetLdnChannel | ausente | ausente | erro CMIF; sem implementação |
| 81 | [S1] SetLdnChannel | ausente | ausente | erro CMIF; sem implementação |
| 82 | AcquireTelemetryDirtyFlagEventHandle | ausente | ausente | erro CMIF; sem implementação |
| 83 | GetTelemetryDirtyFlags | ausente | ausente | erro CMIF; sem implementação |
| 84 | GetPtmBatteryLot | ausente | ausente | erro CMIF; sem implementação |
| 85 | SetPtmBatteryLot | ausente | ausente | erro CMIF; sem implementação |
| 86 | GetPtmFuelGaugeParameter | ausente | ausente | erro CMIF; sem implementação |
| 87 | SetPtmFuelGaugeParameter | ausente | ausente | erro CMIF; sem implementação |
| 88 | GetBluetoothEnableFlag | handler | ausente | implementado |
| 89 | SetBluetoothEnableFlag | handler | ausente | desabilitado; ativação retorna erro |
| 90 | GetMiiAuthorId | handler | handler | erro CMIF; sem implementação |
| 91 | SetShutdownRtcValue | ausente | ausente | erro CMIF; sem implementação |
| 92 | GetShutdownRtcValue | ausente | ausente | erro CMIF; sem implementação |
| 93 | AcquireFatalDirtyFlagEventHandle | ausente | ausente | erro CMIF; sem implementação |
| 94 | GetFatalDirtyFlags | ausente | ausente | erro CMIF; sem implementação |
| 95 | [2.0.0+] GetAutoUpdateEnableFlag | handler | ausente | implementado |
| 96 | [2.0.0+] SetAutoUpdateEnableFlag | handler | ausente | implementado |
| 97 | [S1] [2.0.0+] GetNxControllerSettings | ausente | ausente | erro CMIF; sem implementação |
| 98 | [S1] [2.0.0+] SetNxControllerSettings | ausente | ausente | erro CMIF; sem implementação |
| 99 | [2.0.0+] GetBatteryPercentageFlag | handler | ausente | implementado |
| 100 | [2.0.0+] SetBatteryPercentageFlag | handler | ausente | implementado |
| 101 | [S1] [2.0.0+] GetExternalRtcResetFlag | ausente | ausente | estado vivo de timesrv; por execução |
| 102 | [S1] [2.0.0+] SetExternalRtcResetFlag | ausente | ausente | erro; API de timesrv insuficiente |
| 103 | [3.0.0+] GetUsbFullKeyEnableFlag | ausente | ausente | implementado |
| 104 | [3.0.0+] SetUsbFullKeyEnableFlag | ausente | ausente | desabilitado; ativação retorna erro |
| 105 | [3.0.0+] SetExternalSteadyClockInternalOffset | handler | ausente | erro; API de timesrv insuficiente |
| 106 | [3.0.0+] GetExternalSteadyClockInternalOffset | handler | ausente | estado vivo de timesrv; por execução |
| 107 | [3.0.0+] GetBacklightSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 108 | [3.0.0+] SetBacklightSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 109 | [3.0.0+] GetHeadphoneVolumeWarningCount | ausente | ausente | erro CMIF; sem implementação |
| 110 | [3.0.0+] SetHeadphoneVolumeWarningCount | ausente | ausente | erro CMIF; sem implementação |
| 111 | [S1] [3.0.0+] GetBluetoothAfhEnableFlag | ausente | ausente | implementado |
| 112 | [S1] [3.0.0+] SetBluetoothAfhEnableFlag | ausente | ausente | desabilitado; ativação retorna erro |
| 113 | [S1] [3.0.0+] GetBluetoothBoostEnableFlag | ausente | ausente | implementado |
| 114 | [S1] [3.0.0+] SetBluetoothBoostEnableFlag | ausente | ausente | desabilitado; ativação retorna erro |
| 115 | [3.0.0+] GetInRepairProcessEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 116 | [3.0.0+] SetInRepairProcessEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 117 | [S1] [3.0.0+] GetHeadphoneVolumeUpdateFlag | ausente | ausente | erro CMIF; sem implementação |
| 118 | [S1] [3.0.0+] SetHeadphoneVolumeUpdateFlag | ausente | ausente | erro CMIF; sem implementação |
| 119 | [3.0.0-14.1.2] NeedsToUpdateHeadphoneVolume | ausente | ausente | erro CMIF; sem implementação |
| 120 | [3.0.0+] GetPushNotificationActivityModeOnSleep | handler | ausente | implementado |
| 121 | [3.0.0+] SetPushNotificationActivityModeOnSleep | handler | ausente | implementado |
| 122 | [4.0.0+] GetServiceDiscoveryControlSettings | ausente | ausente | erro CMIF; sem implementação |
| 123 | [4.0.0+] SetServiceDiscoveryControlSettings | ausente | ausente | erro CMIF; sem implementação |
| 124 | [4.0.0+] GetErrorReportSharePermission | handler | ausente | implementado |
| 125 | [4.0.0+] SetErrorReportSharePermission | handler | ausente | implementado |
| 126 | [4.0.0+] GetAppletLaunchFlags | handler | ausente | erro CMIF; sem implementação |
| 127 | [4.0.0+] SetAppletLaunchFlags | handler | ausente | erro CMIF; sem implementação |
| 128 | [S1] [4.0.0+] GetConsoleSixAxisSensorAccelerationBias | ausente | ausente | erro CMIF; sem implementação |
| 129 | [S1] [4.0.0+] SetConsoleSixAxisSensorAccelerationBias | ausente | ausente | erro CMIF; sem implementação |
| 130 | [S1] [4.0.0+] GetConsoleSixAxisSensorAngularVelocityBias | ausente | ausente | erro CMIF; sem implementação |
| 131 | [S1] [4.0.0+] SetConsoleSixAxisSensorAngularVelocityBias | ausente | ausente | erro CMIF; sem implementação |
| 132 | [S1] [4.0.0+] GetConsoleSixAxisSensorAccelerationGain | ausente | ausente | erro CMIF; sem implementação |
| 133 | [S1] [4.0.0+] SetConsoleSixAxisSensorAccelerationGain | ausente | ausente | erro CMIF; sem implementação |
| 134 | [S1] [4.0.0+] GetConsoleSixAxisSensorAngularVelocityGain | ausente | ausente | erro CMIF; sem implementação |
| 135 | [S1] [4.0.0+] SetConsoleSixAxisSensorAngularVelocityGain | ausente | ausente | erro CMIF; sem implementação |
| 136 | [4.0.0+] GetKeyboardLayout | handler | ausente | implementado |
| 137 | [4.0.0+] SetKeyboardLayout | handler | ausente | implementado |
| 138 | [4.0.0+] GetWebInspectorFlag | ausente | ausente | implementado |
| 139 | [4.0.0+] GetAllowedSslHosts | ausente | ausente | erro CMIF; sem implementação |
| 140 | [4.0.0+] GetHostFsMountPoint | ausente | ausente | erro CMIF; sem implementação |
| 141 | [5.0.0+] GetRequiresRunRepairTimeReviser | ausente | ausente | erro CMIF; sem implementação |
| 142 | [5.0.0+] SetRequiresRunRepairTimeReviser | ausente | ausente | erro CMIF; sem implementação |
| 143 | [S1] [5.0.0+] SetBlePairingSettings | ausente | ausente | erro CMIF; sem implementação |
| 144 | [S1] [5.0.0+] GetBlePairingSettings | ausente | ausente | erro CMIF; sem implementação |
| 145 | [S1] [5.0.0+] GetConsoleSixAxisSensorAngularVelocityTimeBias | ausente | ausente | erro CMIF; sem implementação |
| 146 | [S1] [5.0.0+] SetConsoleSixAxisSensorAngularVelocityTimeBias | ausente | ausente | erro CMIF; sem implementação |
| 147 | [S1] [5.0.0+] GetConsoleSixAxisSensorAngularAcceleration | ausente | ausente | erro CMIF; sem implementação |
| 148 | [S1] [5.0.0+] SetConsoleSixAxisSensorAngularAcceleration | ausente | ausente | erro CMIF; sem implementação |
| 149 | [5.0.0+] GetRebootlessSystemUpdateVersion | handler | ausente | erro CMIF; sem implementação |
| 150 | [5.0.0+] GetDeviceTimeZoneLocationUpdatedTime | handler | ausente | estado vivo de timesrv; por execução |
| 151 | [5.0.0+] SetDeviceTimeZoneLocationUpdatedTime | handler | ausente | estado vivo de timesrv; por execução |
| 152 | [6.0.0+] GetUserSystemClockAutomaticCorrectionUpdatedTime | handler | ausente | estado vivo de timesrv; por execução |
| 153 | [6.0.0+] SetUserSystemClockAutomaticCorrectionUpdatedTime | handler | ausente | erro; API de timesrv insuficiente |
| 154 | [S1] [6.0.0+] GetAccountOnlineStorageSettings | ausente | ausente | erro CMIF; sem implementação |
| 155 | [S1] [6.0.0+] SetAccountOnlineStorageSettings | ausente | ausente | erro CMIF; sem implementação |
| 156 | [S1] [6.0.0+] GetPctlReadyFlag | ausente | ausente | erro CMIF; sem implementação |
| 157 | [S1] [6.0.0+] SetPctlReadyFlag | ausente | ausente | erro CMIF; sem implementação |
| 158 | [S1] [8.1.1+] GetAnalogStickUserCalibrationL | ausente | ausente | erro CMIF; sem implementação |
| 159 | [S1] [8.1.1+] SetAnalogStickUserCalibrationL | ausente | ausente | erro CMIF; sem implementação |
| 160 | [S1] [8.1.1+] GetAnalogStickUserCalibrationR | ausente | ausente | erro CMIF; sem implementação |
| 161 | [S1] [8.1.1+] SetAnalogStickUserCalibrationR | ausente | ausente | erro CMIF; sem implementação |
| 162 | [6.0.0+] GetPtmBatteryVersion | ausente | ausente | erro CMIF; sem implementação |
| 163 | [6.0.0+] SetPtmBatteryVersion | ausente | ausente | erro CMIF; sem implementação |
| 164 | [6.0.0+] GetUsb30HostEnableFlag | ausente | ausente | implementado |
| 165 | [6.0.0+] SetUsb30HostEnableFlag | ausente | ausente | desabilitado; ativação retorna erro |
| 166 | [6.0.0+] GetUsb30DeviceEnableFlag | ausente | ausente | implementado |
| 167 | [6.0.0+] SetUsb30DeviceEnableFlag | ausente | ausente | desabilitado; ativação retorna erro |
| 168 | [S1] [7.0.0+] GetThemeId | ausente | ausente | erro CMIF; sem implementação |
| 169 | [S1] [7.0.0+] SetThemeId | ausente | ausente | erro CMIF; sem implementação |
| 170 | [7.0.0+] GetChineseTraditionalInputMethod | handler | ausente | implementado |
| 171 | [7.0.0+] SetChineseTraditionalInputMethod | ausente | ausente | implementado |
| 172 | [7.0.0+] GetPtmCycleCountReliability | ausente | ausente | erro CMIF; sem implementação |
| 173 | [7.0.0+] SetPtmCycleCountReliability | ausente | ausente | erro CMIF; sem implementação |
| 174 | [8.1.1+] GetHomeMenuScheme | handler | ausente | erro CMIF; sem implementação |
| 175 | [S1] [7.0.0+] GetThemeSettings | ausente | ausente | erro CMIF; sem implementação |
| 176 | [S1] [7.0.0+] SetThemeSettings | ausente | ausente | erro CMIF; sem implementação |
| 177 | [S1] [7.0.0+] GetThemeKey | ausente | ausente | erro CMIF; sem implementação |
| 178 | [S1] [7.0.0+] SetThemeKey | ausente | ausente | erro CMIF; sem implementação |
| 179 | [8.0.0+] GetZoomFlag | ausente | ausente | erro CMIF; sem implementação |
| 180 | [8.0.0+] SetZoomFlag | ausente | ausente | erro CMIF; sem implementação |
| 181 | [S1] [8.0.0+] GetT | ausente | ausente | implementado |
| 182 | [S1] [8.0.0+] SetT | ausente | ausente | implementado |
| 183 | [9.0.0+] GetPlatformRegion | handler | ausente | implementado |
| 184 | [9.0.0+] SetPlatformRegion | handler | ausente | implementado |
| 185 | [9.0.0+] GetHomeMenuSchemeModel | handler | ausente | erro CMIF; sem implementação |
| 186 | [9.0.0+] GetMemoryUsageRateFlag | ausente | ausente | implementado |
| 187 | [S1] [9.0.0+] GetTouchScreenMode | handler | ausente | implementado |
| 188 | [S1] [9.0.0+] SetTouchScreenMode | handler | ausente | implementado |
| 189 | [S1] [10.0.0+] GetButtonConfigSettingsFull | ausente | ausente | erro CMIF; sem implementação |
| 190 | [S1] [10.0.0+] SetButtonConfigSettingsFull | ausente | ausente | erro CMIF; sem implementação |
| 191 | [S1] [10.0.0+] GetButtonConfigSettingsEmbedded | ausente | ausente | erro CMIF; sem implementação |
| 192 | [S1] [10.0.0+] SetButtonConfigSettingsEmbedded | ausente | ausente | erro CMIF; sem implementação |
| 193 | [S1] [10.0.0+] GetButtonConfigSettingsLeft | ausente | ausente | erro CMIF; sem implementação |
| 194 | [S1] [10.0.0+] SetButtonConfigSettingsLeft | ausente | ausente | erro CMIF; sem implementação |
| 195 | [S1] [10.0.0+] GetButtonConfigSettingsRight | ausente | ausente | erro CMIF; sem implementação |
| 196 | [S1] [10.0.0+] SetButtonConfigSettingsRight | ausente | ausente | erro CMIF; sem implementação |
| 197 | [S1] [10.0.0+] GetButtonConfigRegisteredSettingsEmbedded | ausente | ausente | erro CMIF; sem implementação |
| 198 | [S1] [10.0.0+] SetButtonConfigRegisteredSettingsEmbedded | ausente | ausente | erro CMIF; sem implementação |
| 199 | [S1] [10.0.0+] GetButtonConfigRegisteredSettings | ausente | ausente | erro CMIF; sem implementação |
| 200 | [S1] [10.0.0+] SetButtonConfigRegisteredSettings | ausente | ausente | erro CMIF; sem implementação |
| 201 | [10.1.0+] GetFieldTestingFlag | handler | ausente | implementado |
| 202 | [10.1.0+] SetFieldTestingFlag | ausente | ausente | erro CMIF; sem implementação |
| 203 | [11.0.0+] GetPanelCrcMode | handler | ausente | erro CMIF; sem implementação |
| 204 | [11.0.0+] SetPanelCrcMode | handler | ausente | erro CMIF; sem implementação |
| 205 | [S1] [13.0.0+] GetNxControllerSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 206 | [S1] [13.0.0+] SetNxControllerSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 207 | [S1] [14.0.0+] GetHearingProtectionSafeguardFlag | ausente | ausente | erro CMIF; sem implementação |
| 208 | [S1] [14.0.0+] SetHearingProtectionSafeguardFlag | ausente | ausente | erro CMIF; sem implementação |
| 209 | [S1] [14.0.0+] GetHearingProtectionSafeguardRemainingTime | ausente | ausente | erro CMIF; sem implementação |
| 210 | [S1] [14.0.0+] SetHearingProtectionSafeguardRemainingTime | ausente | ausente | erro CMIF; sem implementação |
| 211 | [S2] GetWirelessCertificationHtmlFileSize | ausente | ausente | erro CMIF; sem implementação |
| 212 | [S2] GetWirelessCertificationHtmlFile | ausente | ausente | erro CMIF; sem implementação |
| 213 | [S2] GetWirelessCertificationJpegFileSize | ausente | ausente | erro CMIF; sem implementação |
| 214 | [S2] GetWirelessCertificationJpegFile | ausente | ausente | erro CMIF; sem implementação |
| 215 | [S2] GetHighContrastFlag | ausente | ausente | erro CMIF; sem implementação |
| 216 | [S2] SetHighContrastFlag | ausente | ausente | erro CMIF; sem implementação |
| 217 | [S2] GetTextToSpeechFlag | ausente | ausente | erro CMIF; sem implementação |
| 218 | [S2] SetTextToSpeechFlag | ausente | ausente | erro CMIF; sem implementação |
| 219 | [S2] GetTextMagnificationRatio | ausente | ausente | erro CMIF; sem implementação |
| 220 | [S2] SetTextMagnificationRatio | ausente | ausente | erro CMIF; sem implementação |
| 221 | [17.0.0+] GetForceMonauralOutputFlag | ausente | ausente | erro CMIF; sem implementação |
| 222 | [17.0.0+] SetForceMonauralOutputFlag | ausente | ausente | erro CMIF; sem implementação |
| 223 | [S2] GetUsbAudioVolumeSettings | ausente | ausente | erro CMIF; sem implementação |
| 224 | [S2] SetUsbAudioVolumeSettings | ausente | ausente | erro CMIF; sem implementação |
| 225 | [S2] GetTitleIconKeepFlag | ausente | ausente | erro CMIF; sem implementação |
| 226 | [S2] SetTitleIconKeepFlag | ausente | ausente | erro CMIF; sem implementação |
| 227 | [S2] GetBoldTextFlag | ausente | ausente | erro CMIF; sem implementação |
| 228 | [S2] SetBoldTextFlag | ausente | ausente | erro CMIF; sem implementação |
| 229 | [S2] GetSpeechToTextFlag | ausente | ausente | erro CMIF; sem implementação |
| 230 | [S2] SetSpeechToTextFlag | ausente | ausente | erro CMIF; sem implementação |
| 235 | [S2] GetColorFilterType | ausente | ausente | erro CMIF; sem implementação |
| 236 | [S2] SetColorFilterType | ausente | ausente | erro CMIF; sem implementação |
| 237 | [S2] GetPrioritizedOutputAudioDeviceSettings | ausente | ausente | erro CMIF; sem implementação |
| 238 | [S2] SetPrioritizedOutputAudioDeviceSettings | ausente | ausente | erro CMIF; sem implementação |
| 239 | [S2] GetPrioritizedInputAudioDeviceSettings | ausente | ausente | erro CMIF; sem implementação |
| 240 | [S2] SetPrioritizedInputAudioDeviceSettings | ausente | ausente | erro CMIF; sem implementação |
| 241 | [S2] GetTextToSpeechVoiceTypeForUi | ausente | ausente | erro CMIF; sem implementação |
| 242 | [S2] SetTextToSpeechVoiceTypeForUi | ausente | ausente | erro CMIF; sem implementação |
| 243 | [S2] GetLcdFlags | ausente | ausente | erro CMIF; sem implementação |
| 244 | [S2] SetLcdFlags | ausente | ausente | erro CMIF; sem implementação |
| 245 | [S2] GetTvHdrSettings | ausente | ausente | erro CMIF; sem implementação |
| 246 | [S2] SetTvHdrSettings | ausente | ausente | erro CMIF; sem implementação |
| 247 | [S2] IsColorInversionEnabled | ausente | ausente | erro CMIF; sem implementação |
| 248 | [S2] SetColorInversionEnabled | ausente | ausente | erro CMIF; sem implementação |
| 249 | [S2] GetKeyRemapEnableFlagOnQuickSettings | ausente | ausente | erro CMIF; sem implementação |
| 250 | [S2] SetKeyRemapEnableFlagOnQuickSettings | ausente | ausente | erro CMIF; sem implementação |
| 251 | [18.0.0+] GetAccountIdentificationSettings | ausente | ausente | erro CMIF; sem implementação |
| 252 | [18.0.0+] SetAccountIdentificationSettings | ausente | ausente | erro CMIF; sem implementação |
| 253 | [S2] GetDeviceLockPinCodeLength | ausente | ausente | erro CMIF; sem implementação |
| 254 | [S2] GetDeviceLockPinCode | ausente | ausente | erro CMIF; sem implementação |
| 255 | [S2] SetDeviceLockPinCode | ausente | ausente | erro CMIF; sem implementação |
| 256 | [S2] GetDeviceLockEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 257 | [S2] GetDeviceLockStartPenaltyTime | ausente | ausente | erro CMIF; sem implementação |
| 258 | [S2] SetDeviceLockStartPenaltyTime | ausente | ausente | erro CMIF; sem implementação |
| 259 | [S2] GetDeviceLockErrorCount | ausente | ausente | erro CMIF; sem implementação |
| 260 | [S2] SetDeviceLockErrorCount | ausente | ausente | erro CMIF; sem implementação |
| 261 | [S2] GetBatteryCareModeEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 262 | [S2] SetBatteryCareModeEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 263 | [20.0.0+] AcquireVphymDirtyFlagEventHandle | ausente | ausente | erro CMIF; sem implementação |
| 264 | [20.0.0+] GetVphymDirtyFlags | ausente | ausente | erro CMIF; sem implementação |
| 265 | [S2] GetInitialLaunchSettings | ausente | ausente | erro CMIF; sem implementação |
| 266 | [S2] SetInitialLaunchSettings | ausente | ausente | erro CMIF; sem implementação |
| 267 | [S2] GetManufacturingTimeStamp | ausente | ausente | erro CMIF; sem implementação |
| 268 | [S2] SetManufacturingTimeStamp | ausente | ausente | erro CMIF; sem implementação |
| 269 | [S2] GetInputNoiseReductionForCommunicationFlag | ausente | ausente | erro CMIF; sem implementação |
| 270 | [S2] SetInputNoiseReductionForCommunicationFlag | ausente | ausente | erro CMIF; sem implementação |
| 271 | [S2] GetChatTranscriptionSettings | ausente | ausente | erro CMIF; sem implementação |
| 272 | [S2] SetChatTranscriptionSettings | ausente | ausente | erro CMIF; sem implementação |
| 273 | [S2] GetBuiltInMicrophoneGain | ausente | ausente | erro CMIF; sem implementação |
| 274 | [S2] SetBuiltInMicrophoneGain | ausente | ausente | erro CMIF; sem implementação |
| 275 | [S2] GetBuiltInMicrophoneJackGain | ausente | ausente | erro CMIF; sem implementação |
| 276 | [S2] SetBuiltInMicrophoneJackGain | ausente | ausente | erro CMIF; sem implementação |
| 277 | [S2] GetUsbAudioInputDeviceGainSettings | ausente | ausente | erro CMIF; sem implementação |
| 278 | [S2] SetUsbAudioInputDeviceGainSettings | ausente | ausente | erro CMIF; sem implementação |
| 279 | [S2] SetBluetoothStackFlag | ausente | ausente | erro CMIF; sem implementação |
| 280 | [S2] SetHidDebugOcdUsbFlag | ausente | ausente | erro CMIF; sem implementação |
| 281 | [S2] SetHidDebugRailFlag | ausente | ausente | erro CMIF; sem implementação |
| 282 | [20.0.0+] ConvertToProductModel | ausente | ausente | erro CMIF; sem implementação |
| 283 | [20.0.0+] ConvertToProductModelName | ausente | ausente | erro CMIF; sem implementação |
| 284 | [S2] GetSaveDataPurgedForRepairFlag | ausente | ausente | erro CMIF; sem implementação |
| 285 | [S2] SetSaveDataPurgedForRepairFlag | ausente | ausente | erro CMIF; sem implementação |
| 286 | [S2] [20.0.0+] GetAppletParameterSet | ausente | ausente | erro CMIF; sem implementação |
| 287 | [S2] [20.0.0+] SetAppletParameterSet | ausente | ausente | erro CMIF; sem implementação |
| 288 | [S2] [20.0.0+] BindChatTranscriptionSettingsChangedEvent | ausente | ausente | erro CMIF; sem implementação |
| 289 | [20.0.0+] GetDefaultAccountIdentificationFlagSet | ausente | ausente | erro CMIF; sem implementação |
| 290 | [S2] [20.0.0+] GetMouseEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 291 | [S2] [20.0.0+] SetMouseEnableFlag | ausente | ausente | erro CMIF; sem implementação |
| 292 | [S2] [20.0.0+] GetTextToSpeechVoiceVolume | ausente | ausente | erro CMIF; sem implementação |
| 293 | [S2] [20.0.0+] SetTextToSpeechVoiceVolume | ausente | ausente | erro CMIF; sem implementação |
| 294 | [S2] [20.0.0+] GetTextToSpeechVoiceSpeed | ausente | ausente | erro CMIF; sem implementação |
| 295 | [S2] [20.0.0+] SetTextToSpeechVoiceSpeed | ausente | ausente | erro CMIF; sem implementação |
| 296 | [S2] [20.0.0+] GetSleepSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 297 | [S2] [20.0.0+] SetSleepSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 298 | [S2] [20.0.0+] GetMousePointerSpeedScale | ausente | ausente | erro CMIF; sem implementação |
| 299 | [S2] [20.0.0+] SetMousePointerSpeedScale | ausente | ausente | erro CMIF; sem implementação |
| 300 | [20.0.0+] AcquirePushNotificationDirtyFlagEventHandle | ausente | ausente | erro CMIF; sem implementação |
| 301 | [20.0.0+] GetPushNotificationDirtyFlags | ausente | ausente | erro CMIF; sem implementação |
| 302 | [S2] [20.0.0+] GetTvHdrSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 303 | [S2] [20.0.0+] SetTvHdrSettingsEx | ausente | ausente | erro CMIF; sem implementação |
| 304 | [S2] [20.0.0+] GetOunceProControllerMicrophoneJackGain | ausente | ausente | erro CMIF; sem implementação |
| 305 | [S2] [20.0.0+] SetOunceProControllerMicrophoneJackGain | ausente | ausente | erro CMIF; sem implementação |
| 306 | [20.0.0+] GetPinCodeReregistrationGuideAccounts | ausente | ausente | erro CMIF; sem implementação |
| 307 | [20.0.0+] SetPinCodeReregistrationGuideAccounts | ausente | ausente | erro CMIF; sem implementação |
| 308 | [S2] [20.0.0+] GetChatTranscriptionSettings | ausente | ausente | erro CMIF; sem implementação |
| 309 | [S2] [20.0.0+] SetChatTranscriptionSettings | ausente | ausente | erro CMIF; sem implementação |
| 310 | [S2] [20.0.0+] GetDeviceLockPinCodeLsbParity | ausente | ausente | erro CMIF; sem implementação |
| 311 | [S2] [20.0.0+] VerifyDeviceLockPinCode | ausente | ausente | erro CMIF; sem implementação |
| 312 | [S2] [20.0.0+] GetDeviceLockVeificationForbiddenFlag | ausente | ausente | erro CMIF; sem implementação |
| 315 | [21.0.0+] GetHttpAuthConfigs | handler | ausente | erro CMIF; sem implementação |
| 319 | [21.0.0+] GetAccountUserSettings | handler | ausente | erro CMIF; sem implementação |
| 320 | [21.0.0+] SetAccountUserSettings | ausente | ausente | erro CMIF; sem implementação |
| 321 | [21.0.0+] GetDefaultAccountUserSettings | handler | ausente | erro CMIF; sem implementação |
| 324 | [22.0.0+] GetPtmQhClearCount | ausente | ausente | erro CMIF; sem implementação |
| 325 | [22.0.0+] SetPtmQhClearCount | ausente | ausente | erro CMIF; sem implementação |
| 326 | [22.0.0+] GetAirPlaneModeRestoreFlagSet | ausente | ausente | erro CMIF; sem implementação |
| 327 | [22.0.0+] SetAirPlaneModeRestoreFlagSet | ausente | ausente | erro CMIF; sem implementação |
| 328 | [22.0.0+] DeleteSettingsPerAccount | ausente | ausente | erro CMIF; sem implementação |

## Validação

- `git diff --check`.
- `bash tests/settings/run.sh`: código de produção de persistência e helpers de IPC, incluindo reabertura, falha real de gravação, arquivo inválido, limites/canários e tipos do catálogo. ASan/UBSan; neste container, LeakSanitizer requer `ASAN_OPTIONS=detect_leaks=0` devido à indisponibilidade de `/proc/.../task` (sem desabilitar ASan/UBSan).
- Mapas extraídos comparados byte a byte com o arquivo de referência fixado.
- IDs registrados comparados por nome com cada linha Switchbrew; tabela distingue handlers das referências de implementação real.
- Build local Android tentada: bloqueada antes de compilar, sem acesso a `services.gradle.org` para Gradle 8.2. Build Android via CI deve ser conferida na revisão final.
- Validação de inicialização/jogabilidade requer APK no dispositivo; estes testes não executam nnSdk ou clocks Android.
