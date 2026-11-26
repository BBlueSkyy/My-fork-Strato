# #463 — revisão da integração com a master

## Resultado

HEAD auditado: a8268d0d83536ab1c353feef328d7f9e9d967323.
Master incorporada: e8e38237aa5993deeaaae9f01f39ea08500f18db.
Checkpoint funcional anterior: 43ead4b3.
Branch feat/conservative-texture-gc-20261116, mesma PR Draft #463.

A integração preservou o GC conservador e as correções recentes da master.
Foram corrigidos dois bloqueios de harness: uma definição duplicada no teste
Vulkan resultante da integração e uma facade de submissão desatualizada em relação
à #484. Não houve alteração em arquivos de produção, política de coleta,
reproteção, sincronização, shader compiler ou instrumentação.

A PR está tecnicamente apta para integração no escopo conservador testado, com a
limitação preexistente de Free Guest ON já documentada/aceita. A suíte completa de
backing não é declarada verde: ela contém CheckFreedWriterGuestBridge, cuja falha
na master já foi demonstrada na auditoria da #484 e não foi reinvestigada aqui.
Não foi feito merge, não se mudou o estado Draft e não se gerou APK.

## Comparação dos arquivos reconciliados

| Arquivo/caminho | Resultado |
| --- | --- |
| common/trap_manager.cpp/.h | Algoritmo otimizado de reproteção da master preservado; mantém owner weak, pins de callback e recuperação de fault atrasado da #463 |
| gpu/texture_manager.cpp/.h | CollectGarbage e RetainTextureView iguais a 43ead4b3; integração mantém SynchronizeGuestOverlaps da #484 |
| gpu/texture/texture.cpp | Destrutor/cleanup da #463 preservados; RecordGpuWrite, unlock, RegisterAliases, promoção e leitura sparse iguais à master |
| interconnect/common/textures.cpp | OnMappingChange igual ao checkpoint GC; consultas e views de texel buffers iguais à master |
| tests/gpu_backing_promotion/run.py | Extrai os métodos atuais de lock, GC, retenção, destruição e sparse da árvore integrada; sem substituir pela versão antiga |
| tests/gpu_backing_promotion/tests.cpp | Controles GC/lifetime preservados; removida inclusão duplicada de tic_collection.inc |

source-comparison.json registra hashes dos corpos comparados. Em particular,
ReprotectIntervals, TrapRegions e DeleteTrap são idênticos à master e não foram
substituídos pela versão antiga da #463. A política do GC, o tracker físico e a
retenção de views não foram ampliados. Os arquivos completos de pipeline.inc,
pipeline_specialization, pipeline_state_bundle, pipeline managers graphics/compute,
command_executor.cpp e interval_map.h também foram conferidos contra a master.

## Reproteção e lifetime de traps

ReprotectIntervals preserva a fronteira por páginas e a proteção mais restritiva
entre traps que cobrem cada segmento. DeleteTrap remove a entrada antes de
recalcular as páginas, evitando deixar uma página protegida sem callback que possa
resolvê-la. O caminho comum ReadWrite continua direto para PROT_NONE; o sweep de
partial overlaps usa armazenamento inline e coalescência da master.

A correção de 43ead4b3 não foi perdida ao incorporar esse algoritmo mais recente:
traps adjacentes, remoções parciais, callbacks concorrentes e uma proteção sem
entrada remanescente são cobertos pelos testes executados. O teste com SIGSEGV real
retardado retomou a instrução depois da exclusão do último trap, sem tornar faults
unmapped/read-only/NX bem-sucedidos.

HandleTrap retém os owners promovidos até depois de liberar trapMutex, inclusive
nos retries. Texture passa weakThis ao registro. SynchronizeGuest usa RemoveTrap
para desproteger sem destruir o handle reutilizável; o destrutor faz a exclusão
final uma vez e espera o uso da imagem antes de liberar backing/view/framebuffer.
Declarações, assinaturas e chamadas NCE concordam no parâmetro protectionFault.
Nenhuma nova definição duplicada de produção foi encontrada.

## Condições de coleta preservadas

CollectGarbage só retira uma imagem com um mapeamento físico elegível completamente
não residente em todos os AS/VA rastreados, sem overlap de outra representação,
sem AliasGroup, sem pin, sem guest descartado e sem escrita GPU não preservada.
GpuDirty ou gpuWriteSerial != gpuReadbackSerial bloqueiam a coleta. Referências
externas, views/descritores/executor e fences incompletas também a adiam.

O use_count é apenas a última barreira de ownership, depois das provas de dados e
mapeamento. Os locks de recurso e estado são adquiridos com try-lock. Antes da
retirada, o objeto se torna CpuDirty para impedir readback obsoleto por callbacks
que tenham feito weak-promotion. O índice de overlaps e o vetor de sobreviventes
são reconstruídos antes de publicar a remoção. Uma falha de alocação não autoriza
perder a única referência do cache.

A política permanece com até 16 candidatos por lote, sem scan de todas as texturas
por draw/frame quando não houve evento. A fila de views tem inspeção limitada a
64 por coleta. Nenhuma espera GPU/readback novo foi acrescentado à coleta nesta
revisão. Os pins, aliases, mapeamentos físicos fragmentados e HDR parcialmente
sobreposto continuam conservadoramente retidos.

GMMU Map/Unmap atualiza a cobertura apenas após a mutação real. Overflow/falha do
índice fecha a coleta, não a tradução VA. O tracker compartilhado entre ASes e o
lease do batch mantêm a ordem channelLock → mapping mutex → locks GMMU. As mutações
não destroem texturas nem fazem Vulkan. O worker libera a lease no unwind antes
dos handlers de erro. A ordem de destruição mantém caches Vulkan vivos além dos
objetos de textura e dependências da scheduler.

## RetainTextureView e descritores

RetainTextureView usa o mutex interno da textura, sem chamar Texture::unlock no
snapshot. Isso preserva tag e pendingGpuWriteTag da posse do executor, incorporados
pela #484. A queue retém view e os ciclos anterior/corrente, sem anexar dependências
tardiamente a fences já submetidas. Só recebe ciclo corrente se houver comandos
pendentes; ciclos vazios não prendem views indefinidamente por esse caminho.

OnMappingChange invalida cache rápido e store forte de TIC em mudança de geração,
mas retém as views antigas até os dois ciclos relevantes terminarem/cancelarem.
O teste chama o método de produção e verifica que descritores raw já gravados não
perdem seu objeto. Depois da conclusão, a mesma memória/VA é reconstruída com os
novos bytes guest; não se reutiliza uma backing velha por endereço reciclado.

Sparse textures continuam no caminho de reconstrução GMMU/mirror da master,
sem criar uma falsa identidade de alocação física para buracos. O GC não substitui
as regras de materialização sparse nem as dependências Float/UINT/SINT/PCHE7.
As correções MK8/SMO de pending writes/preflight e o caminho CBUF de Sniper Elite3
permanecem no código atual; não se introduziu fallback ou solução específica de jogo.

## Defeitos corrigidos nesta revisão

1. tests/gpu_backing_promotion/tests.cpp incluía tic_collection.inc duas vezes no
   mesmo namespace gc_test. O compilador rejeitava a redefinição do método de
   produção Textures::OnMappingChange. Foi removida somente a segunda inclusão.
2. tests/gpu_submission/tests.cpp não tinha pendingGpuWriteTag na facade Texture,
   embora o SubmitInternal atual o limpe depois de publicar o novo ciclo. Essa
   incompatibilidade já estava na master, não foi criada pelo GC. Acrescentei o
   campo inicializado com recibo não zero e uma assertiva de que ele já é zero
   quando UpdateRenderPassUsage é alcançado. O teste continua executando o corpo
   de SubmitInternal, não uma cópia simplificada da lógica de sequenciamento.

Nenhum teste foi removido/desabilitado. Não foi modificado expected de preservação
para fazer a falha conhecida de Free Guest passar. A montagem inicial do teste de
framebuffer precisou dos paths de headers Vulkan-Hpp e Boost do host; não foi
necessário patch no código para esse ajuste de ambiente.

## Validação na árvore integrada

| Teste | Resultado |
| --- | --- |
| gpu_texture_lifetime/run.py, ASan/UBSan | SIGSEGV real atrasado, 180 interleavings, read/write/retry/exception e 11 cenários de lifetime passaram |
| gpu_texture_lifetime/run_mappings.py, ASan/UBSan | AS/VA múltiplos, partial unmap, VA reuse, exclusão de batch e 1000 mutações concorrentes passaram |
| trap_manager/run.py --sanitize | Reproteção por página; nenhuma proteção órfã; passou |
| Backing TEXTURE_LIFETIME_ONLY, ASan/UBSan + Vulkan sync validation | 44 imagens elegíveis liberadas no total; zero erros de validação |
| Backing TEXMAN_PENDING_ONLY | Receipt, retry, falhas de submissão/readback, locks e CPU-newer OFF/ON passaram |
| Backing TEXMAN_LOOKUP_ONLY | Índice/mapeamentos fragmentados, lookup aninhado e writer contido passaram |
| gpu_submission/run.py, ASan/UBSan | 32 verificações de fence/submissão/staging/ciclo, incluindo recibo limpo após publicação, passaram |
| gpu_tic_validation/run.py e --probe-cache --vulkan-lifetime | 1186 checks TIC + 130 cache; variantes Float→UINT→SINT e 10 pipelines com fence pendente passaram; zero erros Vulkan |
| gpu_sampled_layout/run.py | Layouts, aliases, invalidação de TIC e retomada passaram |
| gpu_sparse_sampled/run.py | Leitura por mirrors, pages protegidas e snapshots instáveis passaram |
| gpu_texel_numeric/views.py | Cache por owner/backing/formato/offset/range e retenção de views passou |
| gpu_texman_multirange/run.py | Mapping match, overlap index e controles associados passaram |
| gpu_framebuffer_cache/run.py, UBSan | Chaves e compatibilidade de attachment/framebuffer passaram |

A lane Vulkan do GC libera duas imagens por configuração format-list e mais vinte
em lotes16+4 por configuração: 44 ao todo. São alocações sintéticas pequenas, não
uma medição de redução de RAM/RSS no Moto G100. Ela também mantém o overlap HDR
640×360 child/parent do MK8 e todos os bytes guest, imagens GPU-dirty, views externas
e uma cópia image→buffer real pendente; só recolhe os recursos elegíveis depois.
Reutilização de views, alias traps, CPU-write-before-submit, registro após readback,
promoção e expansão1→8/seis faces passam nessa mesma lane.

Os testes executam métodos/corpos de produção. Allocator/scheduler/interfaces OS
mantêm as fachadas documentadas; imagens, cópias, fences e validação Vulkan do
backing são reais. Foi usado Clang19 e software ICD, não Turnip/Adreno. LeakSanitizer
desativado (detect_leaks=0); ASan/UBSan ativos. Não se anuncia prova exaustiva de
concorrência no Android nem auditoria de leaks.

## Limites e recomendação

Nenhuma mudança funcional final nesta revisão. A implementação otimizada da master
foi preservada e os controles de páginas do crash anterior passam. O histórico de
sucesso fornecido pelo usuário após43ead4b3 não foi substituído por alegação de
novo gameplay no aparelho. BOTW/RE5/MK8/SMO não foram executados nesta sessão.

O GC continua incapaz de detectar subalocações recicladas dentro de um heap guest
que permaneça mapeado. Não coleta escritores GPU nem representações sobrepostas
sem certificado de preservação. Não se promete resolver o crescimento de RAM do
MK11, nem ganho de FPS/frametime. Não há benchmark Android novo; nesta revisão não
foi acrescida operação/cópia/espera no runtime.

CheckFreedWriterGuestBridge e a limitação GCC dos inicializadores antigos já foram
separados da #484/master. Não foram reexecutados para anunciar falhas conhecidas
como novas nem usados para enfraquecer os controles do GC. A suite completa de
backing não é declarada verde; foram executadas as lanes explícitas acima, todas
com seus controles originais preservados.

CI do HEAD a8268d0d: execução45167096713, PR Build, success. O workflow histórico
texture_lifetime.yml aponta para outra branch e não certifica automaticamente o
GC desta PR; a cobertura foi executada localmente e preservada nos logs. Esta
atualização usa skip ci para não gerar outra APK, como solicitado. Não se anuncia
um CI novo concluído nem se acompanha Actions continuamente.

Recomendação: tecnicamente pronta para integração conservadora, com os limites
acima e a limitação preexistente aceita. Preservar a PR Draft e a decisão de merge
para o usuário. Nenhuma APK adicional, merge ou ampliação do GC foi realizada.
