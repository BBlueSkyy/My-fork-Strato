# PR #476 — revisão final após integração da #484

## Conclusão

Branch fix/texel-buffer-numeric-type-20261122, continuação de 3722d8d6.
Master incorporada: 66e53c6497edff6dd6bec6dc164b683b2c18b4de, que contém a #484.
Atualização por commit com dois pais, preservando a história e a correção original.
Não é merge da PR na master. Nenhuma outra PR foi modificada.

Não foi reproduzido defeito novo que exija mudar o renderer ou o compilador nesta
revisão. Recomendo a #476 para integração no escopo de tipagem/especialização de
texel buffers, com as limitações abaixo. Não se afirma correção do SpongeBob nem
nova validação gráfica ou de desempenho no Moto G100.

O submódulo shader-compiler permanece exatamente em
9cbc37dc943de4fae527036c15352e43921e8b8d. Foram preservadas as correções anteriores
na base desse compilador, inclusive o fallback CAS e o tratamento de handles/CBUF.
Não foi rebaixada a revisão do compilador para acomodar o merge.

## Auditoria dos tipos e valores

- GetTicBufferType classifica os formatos inteiros que ConvertTicFormat pode
  produzir como Uint ou Sint; formatos float e normalizados permanecem Float.
  A view de runtime utiliza a mesma conversão de formato TIC. Não há conversão
  indiscriminada de sampled images para UINT.
- TexturePass consulta BufferType somente para descritores de buffer e preserva
  a origem do handle. A deduplicação considera tipo numérico e os dois shifts;
  compartilhar o mesmo CBUF não basta para fundir descritores semanticamente distintos.
- DefineTextureBuffers cria OpTypeImage com scalar Float/u32/s32 e mantém o tipo
  de resultado de cada descritor. TextureImage carrega a imagem daquele descritor.
- EmitImageFetch usa o vetor tipado correspondente. UINT/SINT retornam aos slots
  F32 do IR por OpBitcast, não conversão numérica. Extensão de sinal/zero e os
  componentes ausentes (inclusive alpha inteiro=1) seguem o fetch Vulkan.
- RelaxedPrecision é suprimido nos fetches inteiros mesmo quando a instrução pede
  precisão reduzida. As regras anteriores de precisão dos caminhos Float e das
  sampled images comuns não foram globalmente alteradas.
- SPIR-V Tools validou os módulos compute e fragment. A validação de módulos não
  certifica o formato da view em runtime; por isso há também dispatches Vulkan.

## Handles e compatibilidade com #471 / Sniper Elite 3

ReadBindlessHandle preserva ReadForShaderCompile para ambas as fontes. Uma fonte
não vinculada contribui zero; não descarta a outra fonte válida. Views truncadas
e leituras com produtor pendente mantêm o unwind/retry da base.

Na fonte única com shift, runtime retorna primary<<shift. HandleSource usa duas
ocorrências da mesma origem com o mesmo shift; a composição OR é idempotente e
produz o mesmo handle. Descritores sem has_secondary continuam no caminho raw
anterior. Fontes secundárias reais mantêm seus próprios shifts e offsets.

Os testes de TexturePass incluem word3/shift2, origens split c[2]/c[16], bound e
bindless, secundário ausente/válido e deduplicação de shifts diferentes. Os testes
host de TIC preservam o secundário quando o primário está ausente e as regras de
limite/submissão da #471. Não houve mudança no fallback que habilitou Sniper Elite3.
Isso é cobertura de semântica de descritores, não novo teste do jogo no aparelho.

## Especialização, cache e preload

Graphics e compute registram TextureBufferType e reconsultam o valor durante
Refresh. As origens/handles são verificadas antes da consulta ao TIC antigo.
Mudança Float→UINT ou UINT→SINT exige variante diferente; dados uniformes não
consultados pelo tradutor não viram dependências de especialização.

RuntimeGraphicsPipelineStateAccessor armazena o metadado no bundle;
FilePipelineStateAccessor o recupera no preload. O bundle preserva as tabelas
anteriores, serializa a tabela numérica e limpa a tabela em Reset.
PCHE Version7 rejeita Version5 e a Version6 experimental. Cache antigo recompila;
não é reutilizado com um layout binário incompatível. Isso pode causar recompilação
inicial de shaders após atualizar, mas não uma perda de conteúdo guest/GPU.

A identidade de BufferView inclui owner/backing, formato, offset e range. A
recriação é coordenada pelo recreationMutex; as views antigas vivem com seu owner,
em vez de se reutilizar um VkBuffer reciclado como identidade de lifetime.
Não foi encontrada necessidade de alterar esse mecanismo nesta revisão.

## Integração com a #484

O merge não teve conflitos de produção. Apenas tests/gpu_tic_validation/tests.cpp
foi combinado automaticamente; foram preservados os contratos da facade introduzidos
na master e os casos numéricos da #476.

As versões finais dos arquivos abaixo são idênticas à master 66e53c64:
command_executor.cpp, maxwell_3d/maxwell_3d.cpp, texture/texture.cpp,
texture/texture.h, texture/alias_group.h e texture_manager.cpp/.h.
Logo, pendingGpuWriteTag, SynchronizeGuestOverlaps, unlock e preflight de aliases
da #484 não foram substituídos ou reinterpretados pela atualização da #476.

O binding de texel buffers mantém AttachBuffer, PopulateReadBarrier e bloqueio de
writes sequenciadas na backing. A especialização numérica seleciona o programa;
não substitui tracking de conteúdo por tipo de shader. O caminho de texel buffers
não passa a registrar uma escrita Texture por consultar metadata de TIC.

Os novos tipos de query foram mantidos em runtime, compute, graphics e preload.
O teste de lifetime cria dez variantes reais, retém pipelines enquanto a fence
anterior está pendente e valida zero erros Vulkan. Não houve alteração em kernel,
scheduler, apresentação, GC ou política Free Guest nesta revisão.

## Testes nesta árvore combinada

| Teste | Resultado |
| --- | --- |
| gpu_texel_numeric/build.py, backend real, ASan/UBSan | Compute e fragment com Float/UINT/SINT, SPIR-V válido, fetch inteiro sem RelaxedPrecision |
| gpu_texel_numeric/vulkan.cpp, ASan/UBSan | 3 grupos de formato, 36 palavras exatas; zero erros de validação |
| Controle all-float existente | Exatamente 2 VUID format-07753; command buffer inválido não é submetido |
| gpu_texel_numeric/bundle.py | PCHE7 round-trip, Reset, rejeição v5/v6; ASan/UBSan |
| gpu_texel_numeric/views.py | Identidades formato/offset/range/backing/owner e retenção de views |
| gpu_tic_validation/run.py | 1186 checks passaram |
| --probe-cache --vulkan-lifetime | 130 checks, variantes graphics/compute Float→UINT→SINT; 10 pipelines retidos; zero erros Vulkan |
| run_shader.py | TexturePass de produção, handles split/shift e 8 targets passaram |
| preload/run.py e replay/test_query_callbacks.py | Lifetime/preload, callbacks e cinco tipos de query passaram |
| gpu_sampled_layout/run.py | Layouts, aliases, RT, TIC cached e retomada passaram com ASan/UBSan |
| Backing TEXMAN_PENDING_ONLY | Recibos, exceções, rollback, dados CPU mais recentes e locks concorrentes passaram OFF/ON |
| Backing TEXMAN_PROMOTION_ONLY | Promoção, mip growth 1→8/seis faces, views cached e comprimidas passaram |

Nos testes Vulkan, ICD host software e layers Khronos, não driver Android. O
harness de backing usa synchronization validation. As fachadas de fronteira de
scheduler/allocator não são um teste completo da thread Android. LSan foi desativado
conforme ambiente (detect_leaks=0); não se anuncia auditoria de leaks.

Os logs finais estão em docs/checkpoints/pr476-final-20261123. A preparação inicial
do host precisou acrescentar spirv-tools ao PATH e os headers Vulkan ao ambiente;
as execuções finais passaram sem patch ou desativação de teste.

## Limitações preservadas e prontidão

CheckFreedWriterGuestBridge já falha na master e na #484; a auditoria anterior
provou a mesma falha com os métodos da base. Essa limitação Free Guest ON permanece
documentada/aceita, sem alteração ou nova tentativa de correção nesta PR. Não foi
repetida a investigação ou anunciada como regressão da #476. A suíte completa de
backing não é apresentada como verde: foram executados os modos focados pertinentes.
As limitações GCC dos inicializadores preexistentes também não foram reabertas;
os testes executáveis desta revisão usam Clang.

Os controles negativos de shifts já preservados no checkpoint original continuam
válidos; a implementação desses shifts não mudou com a atualização. Não se repetiu
a investigação dos efeitos visuais do SpongeBob. O controle negativo Vulkan
all-float foi reexecutado porque testa diretamente a incompatibilidade corrigida.

O CI anterior de 3722d8d6 passou, execução45036943853. A atualização usa skip ci
para evitar APK/compilação Android desnecessária; não se anuncia CI novo concluído.
As verificações acima foram executadas na árvore combinada antes da publicação.
Nenhuma nova instrumentação ou APK foi adicionada.

Pronta para integração técnica no escopo desta PR, respeitando a limitação
preexistente explicitada. MK8/SMO preservam o código da versão B validada, e a
compatibilidade CBUF de Sniper Elite3 permanece coberta pelos controles host.
Não há alegação de novos testes gráficos, FPS ou frametime no aparelho.

## Build ARM64 solicitada após a revisão

Em 2026-11-23, solicitada execução do workflow PR Build sobre esta árvore já auditada. Este acréscimo é somente documental e destina-se a gerar novo evento pull_request/synchronize sem alterar código de produção, testes, versões ou a integração da #484. O resultado da build deverá ser confirmado no GitHub Actions; esta nota não afirma conclusão nem validação no aparelho.
