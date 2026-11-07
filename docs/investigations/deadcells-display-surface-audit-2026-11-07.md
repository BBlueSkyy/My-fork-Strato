# Dead Cells / Strato — auditoria do caminho da superfície (guest → apresentação)

**Base:** HEAD remoto da PR #425 = `b504ec547d35ee7f470023918841c5853efc0686` (inalterado desde o último commit conhecido; `1664a0e` está no histórico).
**Alterações de código de produção:** nenhuma. **Instrumentação nova:** nenhuma. **APK:** nenhum. Nada foi enviado ao remoto (acesso somente leitura); os arquivos novos estão só na árvore de trabalho local.

## 0. Limites desta análise (leia primeiro)

1. **`Diagnóstico.zip` não estava disponível nesta sessão.** Só chegaram o prompt e o relatório `deadcells_memoria_origem_analise_2026-11-07.md`. Todos os números de captura abaixo vêm desse relatório; nada foi reconferido em `probe.jsonl`.
2. **O código do Eden não pôde ser lido.** `git.eden-emu.dev` e `raw.githubusercontent.com` são bloqueados pelo proxy; não encontrei espelho GitHub acessível; a única página de ancestral do yuzu que consegui abrir não continha o código relevante. **A comparação com o Eden está, portanto, não verificada** (seção 5).
3. Os testes nativos desta entrega comprovam **premissas de mecanismo e de forma dos dados**, não a causa ponta a ponta. Não há evidência suficiente para propor alteração gráfica.

## 1. Fluxo da superfície no código (HEAD `b504ec5`)

Caminhos relativos a `app/src/main/cpp/skyline/`.

| # | Etapa | Local | O que faz (verificado) |
|---|---|---|---|
| 1 | Registro NvMap | `services/nvdrv/devices/nvmap.cpp:44-88`; `services/nvdrv/core/nvmap.cpp:15-38` | `Handle::Alloc` só grava flags, alinhamento e `address`. **Não zera, não copia, não produz pixels.** Handle IDs avançam de 4 em 4 (`core/nvmap.h:88`). |
| 2 | Mapeamento GMMU | `services/nvdrv/devices/nvhost/as_gpu.cpp:169-253` | `cpuPtr = TranslateVirtualPointer(h->address + bufferOffset)` (l.208); `gmmu.Map(offset, cpuPtr, size)` (l.217/242). O IOVA aponta direto às páginas guest; não há cópia. |
| 3 | Escolha do buffer de tela | `services/hosbinder/GraphicBufferProducer.cpp:624-640` (`SetPreallocatedBuffer`) | Zera `buffer.texture` e libera a duplicata interna do NvMap (l.637). Cada nova pré-alocação obriga nova resolução da textura. |
| 4 | Criação da textura de tela | `GraphicBufferProducer.cpp:309-373` (`QueueBuffer`) | `GuestTexture` com mapping `nvMapHandle->GetPointer() + surface.offset` (l.368-369) → `TextureManager::FindOrCreate` (l.372). |
| 5 | Cache de texturas | `gpu/texture_manager.cpp:17-…` (varredura de overlaps l.64-157; alias/`unrepresentableMatch` l.131-156, 270-…) | Casamento por mapping + formato + dimensões + tile; layouts não representáveis viram grupo de alias. |
| 6 | Escritor GPU de textura | `gpu/texture/texture.cpp:1173-1205` (`RecordGpuWrite`), chamado em `texture.cpp:1769-1773` (RT), `interconnect/maxwell_3d/maxwell_3d.cpp:402` (DrawTexture), `interconnect/fermi_2d.cpp:150` (destino do blit) | Único caminho que marca a textura como `GpuDirty` e incrementa `gpuWriteSerial`. |
| 7 | Escritores **de Buffer** | `interconnect/maxwell_dma.cpp:21-37,66-72`; `inline2memory.cpp` | Maxwell DMA opera **só** via `gpu.buffer.FindOrCreate`; Maxwell DMA e Inline2Memory não referenciam `Texture` (grep: `inline2memory.cpp` só inclui o header do probe). |
| 8 | Sincronização guest→host | `texture.cpp:1214-1296` (`SynchronizeHost`), `302-466` (`SynchronizeHostImpl`) | Só envia upload se `dirtyState == CpuDirty`. Lê os bytes pelo `mirror` (l.308; criado em l.194-195). |
| 9 | Mirror | `kernel/memory.cpp:498-521` (`CreateMirrors`) | `mremap` das mesmas páginas, `PROT_READ|PROT_WRITE`. **Não passa pelos traps da visão guest.** |
| 10 | Traps | `texture.cpp:200-299` | Escrita da CPU em textura `Clean`/`CpuDirty` → `CpuDirty` (l.271-273); em `GpuDirty` → readback e depois `CpuDirty` (l.296). Existe atalho `enableFastGpuReadbackHack` que marca `Clean` sem readback (l.277-281). |
| 11 | Apresentação | `gpu/presentation_engine.cpp:97-257` | Espera o fence (l.103) → `ReconcileAliases()` (l.108) → `SynchronizeHost()` com `gpuDirty=false` (l.166; default em `texture.h:631`) → `CopyFrom` ao swapchain (l.168). |
| 12 | Liberação do slot | `GraphicBufferProducer.cpp:460-484`, `hosbinder/buffer_slot_release_guard.h` | Guarda de geração (correção `1664a0e`). Só afeta quando o slot é devolvido; **não altera pixels**. |

Observação sobre os handles: 2784 → 2804 → 2824 → 2848 equivalem a 5, 5 e 6 criações de NvMap entre apresentações (passo de 4). O mesmo endereço guest (`0x1817120000`) foi, portanto, registrado de novo a cada frame, e a textura foi reutilizada pelo `FindOrCreate`.

## 2. Respostas às cinco perguntas

**1. Pixels corrompidos ou memória estruturada?** **Confirmado: memória guest estruturada.** Os 64 primeiros bytes de `sample_7_guest.bin` decodificam como palavras de 64 bits coerentes com a própria alocação: `+0x20 = base+0x38`, `+0x30 = base−0x6000000`, `+0x28` dentro do mapeamento do heap guest, `+0x38 = 0xc200`. Isso não ocorre em ruído de pixels. O teste `deadcells_display_sync/test.cpp` (`TestHeaderIsStructuredNotPixels`) fixa essas relações. A conversão de layout não introduz o padrão (relatório, seção 2). Um erro meu durante o teste: usei `0x600000` em vez de `0x6000000` na primeira versão; o teste falhou e foi corrigido.

**2. Existe uma imagem GPU correta que não é usada?** **Não verificado.** O relatório só informa `last_gpu_write_serial=1143` para o upload #8, não para o #7. O registro `upload` do #7 contém `cpu_write_trap_hits`, `last_gpu_write_serial`, `ever_used_as_gpu_writer`, `aliases` e `guest_memory_freed` (`deadcells_upload_probe.h:532-546`). Com `query_probe.py` esses campos saem sem nova captura (seção 6).

**3. O gerenciamento de texturas pode escolher mal, ignorar escritor GPU ou manter alias velho?** **Possível em teoria; não demonstrado para Dead Cells.** Duas lacunas estruturais verificadas no código:
- (a) Escritas de GPU que caem em `Buffer` (DMA, Inline2Memory, SSBO) não chegam ao registro de escritor de `Texture` (`RecordGpuWrite` só é chamado nos pontos da linha 6).
- (b) O upload lê pelo mirror, que ignora traps (teste `TestMirrorBypassesGuestViewProtection`). Em `texture_manager.cpp`, `texture.cpp` e `presentation_engine.cpp` não há referência a `gpu.buffer`/flush de buffers sobrepostos. Se um `Buffer` sobreposto estiver `GpuDirty`, o upload leva os bytes guest antigos.

Nenhuma das duas foi ligada à região `0x1817120000`: o relatório não registra escritor posterior ao `NvMap::Alloc`.

**4. Diferenças Eden × Strato?** **Não verificado** (seção 5).

**5. Qual operação deveria produzir os pixels antes da primeira apresentação?** No hardware, uma escrita da GPU no IOVA mapeado para esse NvMap (item 2 do fluxo). No Strato isso aparece como: draw com RT (`UpdateRenderPassUsage`), DrawTexture, blit 2D (`Texture`), ou cópia via `Buffer` (DMA/Inline2Memory/compute). **Qual delas Dead Cells usa para este frame é desconhecido** com os dados atuais; é exatamente a lacuna "registro prospectivo pós-Alloc" já apontada no relatório.

## 3. Classificação das hipóteses

| Hipótese | Estado | Base |
|---|---|---|
| Bytes são estrutura guest, não pixels de conversão errada | **Confirmada** | Relatório §2 + teste de forma dos dados |
| A conversão deswizzle/upload introduz a corrupção | **Refutada** | `staging.bin` reproduz bytes guest; relatório §2 |
| `NvMap::Alloc` ou o GMMU produzem/copiam pixels | **Refutada (por leitura de código)** | Itens 1-2 do fluxo: só bookkeeping e mapeamento de ponteiro |
| Callbacks de BufferQueue atrasados causam os bytes | **Refutada** | Correção `1664a0e` ativa (122 `release_skipped_stale`); bytes permaneceram |
| Os bytes foram escritos antes de `NvMap::Alloc` | **Confirmada** (conteúdo já presente no snapshot) | Relatório §6.1 |
| Escritor da CPU ARM64/NCE antes do Alloc | **Não verificada** | Fora da instrumentação |
| O jogo apresentou um buffer que nunca renderizou (memória legítima do jogo) | **Não verificada** | Compatível com todos os dados; falta o `last_gpu_write_serial` do #7 |
| Escritor GPU em `Buffer` ignorado pelo upload via mirror | **Não verificada** (mecanismo confirmado, ocorrência não) | Teste de mirror + ausência de flush no código |
| Textura GPU separada (alias não casado) com o conteúdo correto | **Não verificada** | Precisa dos campos `aliases`/`serial` do #7 e de eventos de escrita pós-Alloc |
| `VK_ERROR_OUT_OF_POOL_MEMORY` descartou trabalho de GPU desse frame | **Não verificada** | O log não vincula o erro à textura (relatório §1) |
| Atalho `enableFastGpuReadbackHack` (l.277-281) explica os bytes | **Improvável, não testada** | Ele preserva a imagem da GPU; não faz o upload dos bytes guest |

## 4. Testes nativos desta entrega

`tests/deadcells_display_sync/` (executar com `python3 tests/deadcells_display_sync/run.py`; sem Android, Vulkan ou GPU):

- `TestMirrorBypassesGuestViewProtection`: reproduz a construção de `CreateMirrors` (memfd + `mremap` com `old_size=0`), protege a visão guest com `PROT_NONE` e comprova que acesso pelo mirror **não** dispara o trap, enquanto o acesso pela visão guest dispara (controle).
- `TestHeaderIsStructuredNotPixels`: fixa as relações de ponteiro dos 64 bytes e que nenhum dos 16 pixels RGBA8 resultantes é opaco.

Resultado nesta execução: ambos **PASS** (clang++, `-std=c++20 -Wall -Wextra -Werror -pedantic`).

Limite: não há teste ponta a ponta de `TextureManager`/`Texture`; essas classes dependem de Vulkan e não compilam no host. Um teste de regressão só faz sentido depois que o escritor real for identificado.

## 5. Eden

Nenhuma afirmação sobre o Eden é feita aqui. Pontos a conferir quando o código estiver acessível, com as funções equivalentes do Strato:

- seleção do framebuffer por endereço (equivalente a `QueueBuffer` + `FindOrCreate`, itens 4-5);
- o que o Eden faz quando não acha imagem na cache (upload da memória guest, como `SynchronizeHost`);
- como o Eden trata escrita de GPU que cai em buffer sobreposto antes de ler memória guest para uma imagem (equivalente à lacuna 3b).

## 6. Próximo passo sem nova instrumentação

1. Reenviar `Diagnóstico.zip`, ou rodar localmente `python3 tests/deadcells_display_sync/query_probe.py probe.jsonl 7 8 9`. O que decide:
   - `ever_used_as_gpu_writer=false` e `last_gpu_write_serial=0` no #7 → nenhuma `Texture` com esse mapping foi escritor GPU; a pergunta passa a ser se o jogo renderizou via `Buffer` ou não renderizou.
   - `last_gpu_write_serial>0` no #7 → houve escritor GPU e o upload de bytes guest sobrescreveu a imagem; o foco passa a ser quem tornou a textura `CpuDirty` (`cpu_write_trap_hits`, `aliases`).
2. Só se isso não bastar: o registro prospectivo pós-`NvMap::Alloc` já descrito no relatório anterior.
