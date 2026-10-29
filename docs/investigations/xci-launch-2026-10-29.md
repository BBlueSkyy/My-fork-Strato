# Investigação de inicialização XCI — 2026-10-29

Base remota conferida: `master`, `9935f13349c00d732ae599f4f90866b5b79629f5`.
Branch: `fix/xci-launch-validation-20261029`. Trabalho separado da PR #400, sem merge.
Dispositivo alvo: Moto G100, Snapdragon 870/Adreno 650, Android 12; Turnip e Qualcomm proprietário.

## Nicktoons & The Dice of Destiny

Título `0100668022DF4000`. Arquivo analisado: `emulation(20261029-213812).log`,
7.949 bytes; SHA-256 `d2de2e734150d65f4a881123cf51e960b558c9641c5a34d5794c19092a00ef11`.
O log contém inicialização Vulkan/áudio/certificados, ProgramIndex 0,
construção de três backings RomFS, ausência de atualização externa,
carregamento de zero pipelines gráficos e gravação do cache Vulkan.
A última mensagem da thread EmuMain é do PipelineManager; a última mensagem
absoluta é a gravação do cache numa outra thread. Esta gravação não prova
que a GPU terminou toda a inicialização, nem que começou a executar o jogo.

Não há marcador de NPDM, NSO carregado ou thread HOS principal. Não há sinal
fatal, tombstone, motivo de encerramento Android, erro IPC/SVC ou versão do
commit da APK no trecho fornecido. A ausência desses registros não prova crash.
Não há aparelho conectado, Logcat ou tombstone disponíveis nesta execução.

Foi reproduzida uma falha real capaz de interromper XCI antes de NPDM:
`XciLoader::programNca` ocultava `Loader::programNca`. O construtor selecionava
o programa, mas o resolvedor comum via um programa ausente. O teste mostra que
o resolvedor terminava sem `processExeFs`; `XciLoader::LoadProcessData` rejeitaria
o processo nesse estado. Esse defeito é compatível com o ponto de interrupção
do log, mas não demonstra a causa desse boot: falta uma execução da APK
corrigida no aparelho com a instrumentação e o motivo do encerramento.
**Nicktoons não foi validado nem declarado corrigido.**

## Defeitos comprovados e correções

| Causa / local | Reprodução antes | Correção / comprovação depois |
|---|---|---|
| `loader/xci.h`, campo privado `programNca` ocultando a seleção do `Loader` | XCI sintético reconhece Program; resolvedor comum não recebe ExeFS | Removido campo duplicado. Fixture verifica seleção herdada, ExeFS e RomFS finais |
| `loader/xci.cpp`, construtor: interpretação completa de todos os Program NCAs | Program não selecionado com PFS interno inválido impede abrir o XCI | Modo `ProgramDeferred`, como NSP. Cabeçalhos/seções continuam validados; payload do selecionado é aberto pelo resolvedor. Program selecionado inválido continua falhando |
| `loader/xci.cpp`, tamanho do backing HFS0 raiz | Extensão de hash impossível aceita; tamanho anunciado não cobre partições | `hfs0HeaderSize` é tamanho em bytes do intervalo do cabeçalho coberto por hash, não unidades u32 nem tamanho total. Validado contra imagem; backing raiz cobre bytes restantes a partir do offset. Fixture possui secure além do cabeçalho |
| `vfs/partition_filesystem.cpp` | Aceita offset de arquivo com overflow; faltam limites de tabelas/nomes/leitura exata | Validação por subtração, nomes terminados dentro da tabela, leituras completas, prefixo de hash dentro do arquivo; fixtures PFS0/HFS0 válidas e malformadas |
| `crypto/key_store.cpp`, `ReadPairs` | Última chave sem newline desaparece; espaços/CRLF e comentários atrapalham leitura | Processa EOF, trim ASCII, linhas vazias e comentários inteiros #/;. Testes com chaves sintéticas e arquivo OS real |
| `crypto/key_store.cpp`, `PopulateKeys` | `stoul` aceita sufixo parcial/sinal em revisão reconhecida | Exige dois dígitos hexadecimais reais, mantém limite de 32 slots |
| `vfs/nca.cpp`, `GetTitleKey`/`GetKeyAreaKey` | Revisões codificadas 0x21/0xFF abortam com índice fora do array | Verifica geração antes do acesso e retorna MissingTitleKek/MissingKeyArea. Quatro fixtures criptografadas; Application/Ocean/System e title key |
| `loader/nso.cpp`, `GetSegment`/`LoadNso` | LZ4 inválido/curto, compressed size zero e short read aceitos; tamanhos enormes chegam à alocação | Valida flag separadamente, intervalo, limites int do LZ4, leitura exata e retorno exatamente igual ao tamanho esperado |
| `loader/nso.cpp`, metadados .ro | Soma de dynsym/dynstr transborda; diagnóstico lê além do vetor | Limites por subtração e caminho de módulo limitado ao comprimento válido. ASan reproduziu overread de cabeçalho de quatro bytes e strlen de caminho sem terminador |
| `vfs/npdm.cpp/.h`, META/ACI0/KAC | Seções escapam do pai, leituras curtas e ThreadInfo duplicado aceitos; estado ausente não inicializado | Limites hierárquicos, leitura exata, estado inicializado, descriptor único obrigatório e erros explícitos de afinidade. 17 verificações; flags/prioridades válidas preservadas |

O antigo `RegionBacking` encaminha leituras ao pai sem impor seu tamanho
anunciado. Por isso multiplicar o tamanho do cabeçalho por quatro não era,
sozinho, prova de leitura quatro vezes maior nem da falha de Nicktoons.
O conserto distingue a extensão do hash da extensão do filesystem; não altera
os backings globais. Nenhuma verificação criptográfica existente é desativada.
As validações de HFS0 são estruturais, **não** nova autenticação de suas hashes.

## Instrumentação

`loader/startup_trace.h` registra `stage`, `status` e `us` em entrada,
conclusão, unwind de exceção e saída incompleta. Android recebe uma cópia
síncrona em `StratoStartup`, além da fila de logs existente. O limite JNI
registra a mensagem da exceção diretamente no Logcat. Não instala handlers
novos, não captura SIGKILL e não substitui tombstones.

Etapas: GetLoader, ResolveProgramContent, GPU.Initialise, KProcess.create,
LoadProcessData, NPDM de XCI/NSP/NCA, abertura de rtld, VMM, cada NSO e segmento,
análise NCE, mapeamento, patch NCE, regiões finais, heap/TLS, criação da thread,
inicialização do modo de execução e despacho inicial do guest.
Os escopos internos terminam antes de execução que pode usar longjmp.
`MainThread.runUntilExit` abrange a vida da thread: ausência de `ok` enquanto
o jogo está rodando é esperada. `MainThread.dispatchGuest` comprova o despacho,
não que uma instrução guest já foi executada.

Para a validação externa, iniciar captura antes de abrir o jogo, por exemplo:

```sh
adb logcat -b main -b system -b crash -v threadtime > nicktoons-logcat.txt
# Em outro terminal, após reproduzir:
adb shell dumpsys activity exit-info org.stratoemu.strato > nicktoons-exit-info.txt
```

Guardar também `emulation.log` completo, versão/commit da APK, driver e se a
aplicação fecha ou fica bloqueada. Tombstones/bugreport, quando acessíveis,
distinguem sinal nativo, ANR e morte por memória. Um último `enter` localiza uma
etapa pendente; sem motivo Android ele não distingue bloqueio de morte abrupta.

## Comparações técnicas verificáveis

Fontes consultadas para semântica e arquitetura, sem copiar implementações:

- Eden/Yuzu, revisão `a2aa4d97e10a8345babfe647e1abd5ed5909c623`:
  [card_image.cpp](https://github.com/eden-emulator/mirror/blob/a2aa4d97e10a8345babfe647e1abd5ed5909c623/src/core/file_sys/card_image.cpp),
  [card_image.h](https://github.com/eden-emulator/mirror/blob/a2aa4d97e10a8345babfe647e1abd5ed5909c623/src/core/file_sys/card_image.h),
  [nso.cpp](https://github.com/eden-emulator/mirror/blob/a2aa4d97e10a8345babfe647e1abd5ed5909c623/src/core/loader/nso.cpp).
  XCI usa tamanho restante da imagem e encaminha secure ao carregador NSP;
  NSO distingue a flag de compressão e confere resultado LZ4.
- Ryujinx/Ryubing, revisão `475615f0431b4995b03c930c5089d20f6f3570e0`,
  repositório obtido por Git em `https://git.ryujinx.app/projects/Ryubing/`:
  `src/Ryujinx.HLE/Loaders/Processes/ProcessLoader.cs`,
  `Extensions/PartitionFileSystemExtensions.cs`, `Extensions/NcaExtensions.cs`,
  `src/Ryujinx.HLE/Loaders/Executables/NsoExecutable.cs`.
  Secure abre pelo mesmo caminho de partições; CNMT seleciona tipo/ProgramIndex;
  ExeFS/RomFS abrem base/patch sob demanda; NPDM e leitura de segmentos propagam
  erros de LibHac. O projeto fixa `Ryujinx.LibHac 0.21.0-alpha.133`.
  A interface web bloqueou leitura automatizada, mas o clone Git da origem
  foi bem-sucedido. Não houve inspeção interna completa desse pacote LibHac.
- Atmosphere, revisão `6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`:
  [ldr_process_creation.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/stratosphere/loader/source/ldr_process_creation.cpp),
  [ldr_meta.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/stratosphere/loader/source/ldr_meta.cpp),
  [kern_k_capabilities.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/source/kern_k_capabilities.cpp).
  Confere leitura exata, comprimentos LZ4 e limites de NPDM/capacidades.

## Compatibilidade moderna e limites

Esta alteração melhora a ligação XCI ao resolvedor existente e a rejeição
segura de entradas inválidas. Não adiciona formato de gamecard, revisão de
chave, firmware, serviço ou codec novo. ZBIC já existia e continua ativo;
a fixture usa bloco raw válido, não comprova todas as tabelas de entropia BIC.
Sparse NCA, compressed NCA, AES-XTS/CTR/CTR-Ex, BKTR e reconstrução de updates
já existiam no master; o conjunto VFS verifica seus casos sintéticos existentes.
Não é certificação de todos os arquivos modernos.

Permanecem: ausência de autenticação completa ACID/assinaturas NPDM e das
hashes de todos os segmentos NSO; capacidades NPDM não interpretadas;
limites de quatro cores; sem evidência para implementar T2 novo ou mudar NCE.
Meta/Control e outros conteúdos continuam seguindo seus caminhos atuais;
ProgramDeferred não passa a ignorar todo NCA adicional malformado.
Não foram fornecidos executáveis comerciais, dump de cabeçalho/NPDM/NSO do
título ou matriz de lançamentos XCI/NSP no aparelho.

### Horizon OS

`ISystemSettingsServer::GetFirmwareVersion2` mantém perfil 9.0.0-4.0. O log
não mostra consulta desse serviço por Nicktoons. Subir o número anunciado não
implementaria funcionalidades novas.

Auditoria limitada de caminhos concretos: AM contém stubs de gravação e
crash-report e respostas constantes de tamanhos; FSP tem restrições de rank,
índice e storage Host; `ldr:ro` não autentica NRR e restringe espaço 32-bit;
SVCs ausentes falham explicitamente; Time inicializa relógios HLE com o host;
HID contém stubs LR assignment; Account é offline com usuário sintético e
cache de token zerado. Não há evidência de que esses caminhos causem esse boot.

Há ainda uma limitação preexistente em `services/base_service.cpp`:
comando não cadastrado gera warning e retorna sucesso vazio. Settings possui
seu próprio erro UnknownCommand. Essa política geral não foi expandida nem
utilizada como correção; alterá-la em todos os serviços exigiria uma investigação
e validação de compatibilidade própria. Nenhum novo sucesso falso foi adicionado.

## Verificação e riscos

Comandos reproduzíveis em `tests/xci`, `tests/vfs`, `tests/keys`, `tests/npdm`,
`tests/nso` e `tests/startup`. Testes chamam os parsers reais com shims de
plataforma e fixtures sintéticas; não reimplementam os parsers. Bibliotecas
reais de AES, LZ4 e ZBIC são usadas onde indicado nos READMEs.

- XCI/HFS0/PFS0, seleção, validação diferida e geração de chaves: suíte XCI.
- Base/update, múltiplos ProgramIndex, sparse/compressed/BKTR/integridade: VFS.
- KeyStore: oito grupos em um teste CTest; ASan/UBSan também executados.
- NPDM: 17 verificações sob ASan/UBSan.
- NSO: 15 casos normais; 12 sob ASan/UBSan; três casos de tamanho gigante
  usam limite de memória no teste normal para não competir com o VA do ASan.
- StartupTrace: ciclo de conclusão, unwind e escopo incompleto.
- Settings: persistência, erros de escrita, IPC e tipos — passa.
- FSP existente: `proxy validation and state: cache size returned fabricated output`
  falha tanto na branch quanto no master intacto, em diretório separado. Não
  é atribuída a estas mudanças nem ocultada. Os outros nove grupos passam.

LeakSanitizer não funciona sob o tracing deste host; apenas detect_leaks é
desabilitado nos testes sanitizados. CMake 4 exige
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` para a dependência mbedTLS antiga; não foi
alterado o sistema de build Android por esse detalhe do host.

Não há execução de jogos reais ou medição de FPS. Zelda LA, FighterZ e SpongeBob
continuam pendentes de validação no aparelho. Nenhuma espera global, mudança
GPU/TexMan/shader/pipelines/Free Guest ou alteração de sincronização foi feita.
A instrumentação roda no carregamento, não no processamento de cada frame.
O resultado ARM64 DevRelease e os artefatos ficam registrados na PR/Actions.

## Atualização após teste no Moto G100 e limpeza para merge

O novo log enviado em 2026-10-29 comprova resolução de conteúdo, NPDM,
carregamento e LZ4 de rtld/main/subsdk0/sdk, mapeamento e patches NCE,
criação/despacho da thread principal e acesso ao RomFS por IPC.
Portanto a falha anterior de carregamento XCI foi ultrapassada neste teste.
Não equivale a compatibilidade completa do jogo.

Após cerca de 8,8 segundos de execução da thread principal há SIGSEGV
no endereço 0x38, na thread Sky-CmdRecord, com topo simbolizado em
SetDescriptorSetCmdImpl<true>::Record, no caminho de push descriptors.
A causa exata e o objeto inválido ainda não foram identificados.
O erro de áudio anterior não foi demonstrado como causal.

Por solicitação do usuário, os marcadores temporários StartupTrace e suas
cópias síncronas Logcat foram removidos antes do merge, junto do teste
e do plano específicos da instrumentação. As correções e testes de
XCI/NCA/NSO/NPDM/KeyStore e a configuração de repositórios Gradle permanecem.
As seções anteriores de instrumentação/ausência de teste descrevem o estado
inicial da investigação; esta atualização registra o resultado posterior.
O crash de GPU permanece pendente e não recebe correção especulativa nesta PR.
