# PR453 — bloqueio real dos históricos HDR parcialmente sobrepostos

**Conclusão:** o log confirma que a consolidação foi impedida por históricos que
não são doadores de mips inteiros. Não demonstra que esses históricos perderam
sua autoridade. Remover a recusa produz leitores incoerentes nos testes Vulkan,
inclusive para uma imagem com readback concluído ou sem escrita GPU própria.
Não foi demonstrada uma correção local segura para o caso real. O renderer,
shaders, sincronização e diagnósticos Android permanecem intactos; esta revisão
acrescenta testes e documentação, sem novo APK e sem merge.

## Proveniência e limites

Li integralmente as 318 linhas / 33452 bytes do anexo antes de qualquer alteração.
SHA256: `0005a81794db80835942818cb0b89827bdeb909199ec0ce9436d382361f94337`.
A análise usa o head remoto confirmado `619454720342c58578a264295c6994a3d47418d4`.
Ele já contém o master consultado, `f98c9bb6b80a2bc145e07058176b82d9917ae213`.
O anexo não registra o SHA da build: a correspondência é feita com os callsites e
mensagens do checkpoint indicado, sem alegar autenticação do binário pelo log.

[Todos os 40 eventos, com linhas originais](checkpoints/pr453-partial-overlap-20261115/events.txt)
e [inventário/intervalos em JSON](checkpoints/pr453-partial-overlap-20261115/events.json).
O log integral foi preservado localmente em
`refs/checkpoints/pr453-device-log-20261115`; o checkpoint de código inicial em
`refs/checkpoints/pr453-before-partial-audit-20261115`.

O dispositivo registra Turnip Adreno650, Vulkan1.3.280/driver24.0.99, MK8v1.0.0.
Não registra a configuração Free Guest Texture Memory; `donor_freed=false` não
substitui a leitura dessa opção. Os eventos são once-per-texture/classe, filtrados
por formato/geometria, com orçamento96. Foram emitidos40; não houve esgotamento
visível, mas a deduplicação oculta repetições e evolução posterior dos estados.
Não são um histórico completo de todos os draws, escritas ou fences.

## Cronologia completa dos eventos emitidos

Os timestamps abaixo são os valores brutos do logger.

| Linhas | Timestamps | Relação com o código e resultado |
| --- | --- | --- |
| 278–280 | 11320702–11322451 | `FindOrCreate` cria640/1 em`0x86bcbd000`,983040bytes, objeto`0xb4000074bf967388`; RTgen1/serial4892/rp39 e sampledgen1/serial4893/rp43. Episódio em outro intervalo. |
| 281–284 | 11431214–11431234 | Quatro representações menores no mesmo início são recusadas como prefixos desse pedido640/1. |
| 286–288 | 43557445–43560896 | Criação640/1 em`B=0x86bb4d000`, objeto`0xb4000074bfdc8788`; RTview`0xb4000074413672b8`,gen1/serial242931/rp25; sampledview`0xb400007441337428`,gen1/serial242932/rp29. |
| 289–292 | 44039304–44039322 | Quatro imagens menores emB recusadas como prefixos de um pedido640/1; mesmas identidades depois vistas entre os bloqueadores. |
| 293 | 44538219 | `CanExpandGuestPrefix` aceita640/1→640/2:983040→1310720bytes, BH16/spacing0. É elegibilidade, não promoção. |
| 294–295 | 44538238–44538240 | **Primeiro** são recusadas as imagens5mips e1mip em`0x86bc4d000`. |
| 296 | 44538242 | **Depois** é aceito o doador`0xb4000074bfe52bd8`,327680bytes em`0x86bc3d000`,baseMip1,pending=true. |
| 297–300 | 44538243–44538248 | Recusadas mais quatro imagens pequenas no inícioB. |
| 301–304 | 44538250–44538254 | Recusadas quatro imagens maiores que abrangem o pedido, com hint`format_or_image_flags`. |
| 305–306 | 44538255–44538256 | `GetMipDonors` retorna conflito; `FindOrCreate` toma`FALLBACK_TO_SEPARATE_PARENT`. Não chega ao submit/retry de promoção. |
| 307–309 | 44541117–44542047 | Cria parent independente`0xb4000074bfe50958`,1310720bytes/2mips,`overlapping_candidates=8`; sampledview`0xb400007441460a58`,range0+2,gen1,serial0/rp31; depois RTview`0xb40000744145fbe8`,range0+1,gen1,serial266340/rp33. |
| 310–312 | 49232798–49236302 | Outro640/1 em`0x86ba4d000`, objeto`0xb4000074bfe72018`,983040bytes; RTgen1/serial483667/rp7, sampledgen1/serial483668/rp11. |
| 313–316 | 49251142–49251170 | Quatro prefixos menores recusados para esse outro640/1. |
| 317–318 | 52158586–52158643 | Novas identidades5/1mips **agora emB**, recusadas como prefixo de pedido640/1. Não são os objetos de294/295 e não provam mudança de endereço daqueles objetos. |

Contagem:4NEW_IMAGE,4RENDER_TARGET,4SAMPLED,14PREFIX_REJECT,
1PREFIX_ACCEPT,10DONOR_REJECT,1DONOR_ACCEPT,1CONSOLIDATION_BLOCKED,
1FALLBACK_TO_SEPARATE_PARENT; zeroPROMOTION_DONE/VIEW_REFRESH.
`donors_examined=1` é **donors.size(), o número aceito**, e não o total examinado.
São11 identidades de candidatos a doador registradas nesta tentativa, além do
prefixo; não se interpreta esse contador como “só um histórico foi analisado”.

## Intervalos físicos e motivo estrutural

PedidoP=`[0x86bb4d000,0x86bc8d000)`; mip0 termina em`0x86bc3d000`.
Intervalos semiabertos, em bytes. A coluna final mede bytes fora deP.

| Objeto (sufixo do ponteiro) | Início | Fim | Mips | Tamanho | Interseção comP | Fora deP | Resultado |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| `bfdd9738` | `0x86bc4d000` | `0x86bcb8000` | 5 | 438272 | 262144 | 176128 | Rejeitado: não é mip inteiro do parent |
| `bfddd398` | `0x86bc4d000` | `0x86bc9d000` | 1 | 327680 | 262144 | 65536 | Rejeitado: início dentro do mip1 e extensão além deP |
| `bfe52bd8` | `0x86bc3d000` | `0x86bc8d000` | 1 | 327680 | 327680 | 0 | Doador inteiro aceito no mip1 |
| `bfdcae58` | B | `0x86bb4d800` | 1 | 2048 | 2048 | 0 | Subintervalo do mip0 existente |
| `bfddb118` | B | `0x86bb4e800` | 1 | 6144 | 6144 | 0 | Subintervalo do mip0 existente |
| `bfdd8e98` | B | `0x86bb52000` | 1 | 20480 | 20480 | 0 | Subintervalo do mip0 existente |
| `bfdde4d8` | B | `0x86bb61000` | 1 | 81920 | 81920 | 0 | Subintervalo do mip0 existente |
| `bf8e1888` | `0x86bacd000` | `0x86bcad000` | 1 | 1966080 | 1310720 | 655360 | AbrangeP; incompatibilidade de formato/flags indicada |
| `bfd5d328` | `0x86ba4d000` | `0x86be0d000` | 1 | 3932160 | 1310720 | 2621440 | Idem |
| `bfd6da38` | `0x86ba4d000` | `0x86be0d000` | 1 | 3932160 | 1310720 | 2621440 | Idem, outra identidade |
| `bfd03718` | `0x86ae9d000` | `0x86be9d000` | 1 | 16777216 | 1310720 | 15466496 | Idem |

Os dois parciais começam65536bytes após o início do mip1. UmVkImageView seleciona
mips/layers, não um intervalo arbitrário de bytes dentro de um mip. O parent
solicitado tampouco representa suas extensões além deP. Os cinco mips não cabem
nos dois níveis do parent. As quatro imagens pequenas cobrem o mip0, que o
algoritmo preserva do prefixo; elas não são novos mips disjuntos. As quatro
maiores também não cabem no parent. Nenhuma dessas dez imagens pode ser migrada
pelo simples offset de baseMipLevel usado para o doador válido.

`reason_hint` não é o primeiro predicado que falhou. Por exemplo, nos
PREFIX_REJECT de pedidos1mip, old_mips>=req_mips já viola a expansão de cadeia,
independentemente de BH. Para os parciais, o endereço/tamanho já impede a admissão
como mip completo; não é necessário inventar um mismatch específico de GOB.
O hint`format_or_image_flags` não informa qual formato ou flag difere.

## Autoridade: o que é conhecido e o que falta

- Prefixo: teve uso RT e sampled, serial registrado242931/242932,gen1. Parent novo:
  sampled antes do primeiro RT registrado, serial0 e depois266340,gen1. Isso prova
  objetos distintos nesta tentativa, não o congelamento dos pixels deste log.
- Doador válido: aceito por geometria/format/layout e anexado ao mesmo contexto
  admitido pelo algoritmo. `pending=true` vem de`pendingUsageContext`, que é
  marcado para **qualquer AttachTexture**, não só escrita. Não prova sozinho uma
  escrita pendente, nem informa o estado da fence/submissão.
- Os dez recusados: pending/shared/external/freed/alias=false nos registros.
  **Não há seus gpuWriteSerial/gpuReadbackSerial, dirtyState, gerações, views
  vivas, ranges efetivamente escritos ou fences.** A criação e os usos dessas
  imagens não passam pelo filtro640×360, portanto não aparecem comoNEW_IMAGE/RT.
- Sem AliasGroup não existe geração de autoridade compartilhada. Não há recusa
  registrada como`donor_retired`; nenhuma transferência de autoridade desses dez
  históricos foi demonstrada pelo anexo.
- `gpuWriteSerial` é porTexture (`RecordGpuWrite()` não recebe mip/range).
  `gpuReadbackSerial` indica writeback dessa imagem, sem invalidar futuramente
  imagens parcialmente sobrepostas. `backingGeneration` identifica substituição
  de backing, não frescor. Views/caches vivos precisam acompanhar futuras escritas.

Não se confirmou que os dez sejam escritores atuais: confirmou-se que não são
mips disjuntos migráveis. **Nenhuma recusa do log foi comprovada dispensável.**
O guard pode ser conservador para um histórico realmente morto ou já transferido,
mas provar essa condição e retirar todos os leitores/escritores futuros é uma
obrigação; pending=false ou write_serial=0 não a satisfazem. O possível caso de
histórico já aposentado ainda indexado não é o motivo registrado nesta execução.

## Por que os mecanismos atuais não bastam

`GetMipDonors`(texture_manager.cpp:20) marca conflito para qualquer história não
admitida por`MipDonorLevel`(texture.cpp:1460). `PromoteGuestPrefix`(:75) e
`PromoteBackingForView`(texture.cpp:1574) só sabem copiar mips inteiros disjuntos,
migrar suas views e retirar seus traps. É necessário preservar esse contrato.

`CanReconcileGuestLayout`(texture.cpp:947) exige mappings ordenados **idênticos**.
`RegisterAliases`/`ReconcileAliases` mantêm um escritor/generation por grupo, sem
ownership de intervalos/submips. Unir esses spans diferentes ao grupo atual pode
sobrescrever bytes mais recentes ou perder a parte exterior ao parent. Layout
reversível para seu próprio guest não torna iguais seus intervalos físicos.

No fallback, `matches.size()==8` é outra lista, não a lista de doadores. Os oito
spans prefixo+quatro pequenos+dois parciais+doador não contêmP e entram no ramo
`!contains`; os quatro grandes contêmP e percorrem a busca de subranges. Não se
pode usar “8” para dizer que só oito históricos existiam. O fallback faz
`SynchronizeGuest(false,true)` dos matches e cria outroVkImage; isso é uma
materialização pontual, **não reconciliação persistente por intervalo**.

Atualizar descritores, reter fences ou marcar replaced não transfere pixels entre
imagens independentes. Manter simplesmente os backings secundários vivos evita
use-after-free, mas deixa suas views desatualizadas após a próxima escrita.

## Experimentos com dados Vulkan

Novos casos em`CheckLoggedPartialHistories`, executados na suíte normal e pelo
seletor host`TEXMAN_PARTIAL_HISTORIES=1`. São30 combinações: cinco configurações
(apenas doador válido; parcial5; parcial1; ambos; todos os dez spans recusados),
três estados de autoridade (GPU dirty; readback concluído; somente upload CPU) e
dois modos de image-format-list.

Os intervalos/tamanhos/mips são os do log, com base relocada em shared memory.
O anexo não fornece os TICs/geometrias completas dos secundários. A fixture usa
**explicitamente layouts sintéticos consistentes com os tamanhos**, em especial
320×180/5mips e320×256/1mip HDR. Não afirma que esses sejam os TICs reais; formatos
e geometria dos quatro spans maiores também são escolhas do teste. Esses campos
faltantes impedem uma reprodução bit-a-bit do estado Android, mas não alteram a
prova de que os intervalos cortam um mip e ultrapassam o parent.

Usa manager/promoção/sincronização reais extraídos, imagens eviews Vulkan,
render-pass writes, submissions/fences, readbacks nativos e comparações de todos
os mips. Essas comparações certificam o conteúdo texel linear das imagens nativas,
não todo padding GOB da memória guest. Traps/executor/allocator são as facades existentes. O hitTIC e a decisão
de descritores são extraídos; **não executa os shaders do MK8 nem um draw Android**.

Resultados positivos, com o renderer inalterado:
- Só o doador inteiro: promoção preserva mip0 e escrita do antigo RT pendente;
  views/descritores passam ao backing ampliado.
- Com os parciais/menores/maiores: recusa antes de alocar/gravar/submeter a promoção,
  inclusive com falhas de preparação armadas; todos os backings e bytes de todos
  os mips continuam preservados, assim como as views retidas e o trabalho pendente.
- Após concluir a submissão, o fallback permanece independente. O teste não chama
  isso de solução de coerência; comprova preservação das imagens anteriores.
- A suíte existente cobre falhas durante preparação da promoção válida, rollback,
  CPU traps concorrentes, aliases, descritores e lifetime.

Controle destrutivo **somente no corpo host temporário**:
`TEXMAN_TEST_IGNORE_NON_MIP_HISTORIES=1` troca apenas a recusa de não-doadores por
return na fixture; nunca muda arquivos/runtime Android. A promoção passa a ocorrer.
Após nova escrita no mip1, o hitTIC secundário continua devolvendo a imagem antiga.
Uma comparação de pixels reais correspondentes ao mesmo byte físico falha com
`PARTIAL_OVERLAP_STALE_READER`, com zero erros de validação Vulkan antes da falha.
O ponto comparado é texel válido nos dois layouts, não padding de GOB.

Falhas esperadas observadas para5mips: GPUdirty(serial8/readback0), readback
concluído(8/8) e upload-only(0/0, sem reset artificial dos contadores). Também
reproduzida separadamente para o parcial1mip após readback. Portanto “não está
pendente”, “já foi descarregado” ou “não escreveu na GPU” não autoriza ignorar
uma representação que continuará acessível depois de novas escritas sobrepostas.

Comandos reproduzíveis, após configurar Vulkan/Vulkan-Hpp/Boost e o toolchain:

```sh
python tests/gpu_backing_promotion/run.py
TEXMAN_PARTIAL_HISTORIES=1 python tests/gpu_backing_promotion/run.py
# Deve falhar por conteúdo incoerente, não é um candidato de renderer:
TEXMAN_PARTIAL_HISTORIES=1 TEXMAN_PARTIAL_OWNERSHIP=1 \
  TEXMAN_TEST_IGNORE_NON_MIP_HISTORIES=1 python tests/gpu_backing_promotion/run.py
```

## O que permitiria reconciliar, e por que não há novo APK

Uma solução geral precisa preservar autoridade **por intervalo físico dentro do
mip**, incluindo os bytes fora deP: sequenciar os escritores pertinentes, mapear
cada layout reversível, transferir somente as regiões necessárias e invalidar
ou atualizar todos os leitores atingidos, inclusive futuras consultas a views
retidas. Essa informação deve sobreviver a submissions, CPU traps, destruição e
falhas. Os campos globais deTexture e o AliasGroup atual não oferecem esse modelo.
Alternativamente, uma representação pode ser retirada se for comprovadamente
inacessível e sua autoridade inteira já estiver preservada, sem descartar sua
extensão exterior. O log não demonstra isso para nenhuma das dez recusadas.

Falta, para classificar os históricos **reais**: geometria/format/flags completos,
subrecursos e intervalos escritos, serial+readback+dirtyState correlacionados,
owner/cycle de comandos pendentes, leitores vivos e prova de preservação ou
transferência dos bytes exteriores. Identificar “qual é o escritor mais recente”
com um único serial global não resolve a divisão de autoridade.

Não acrescentei diagnóstico nem outra build para buscar esses dados. A análise
existente já basta para rejeitar o relaxamento simples. A captura445 relaciona
3F6EAFE7 ao antigo child congelado em outra execução; este log não tem hashes de
shaders/readbacks e não permite atribuir seus SAMPLED ao início do bloom nem
reutilizar identidades Vulkan entre execuções.

Mantém-se a Draft453. Publicam-se apenas testes/evidência com`[skip ci]` para não
produzir um novo APK sem correção fundamentada. Não se declara CI Android ou
artefato novo aprovado. Os diagnósticos já existentes ficam temporariamente
preservados. O defeito visual e sua atribuição exata à341 continuam abertos.

## Verificação final local

A suíte nativa completa passou após incorporar os novos casos; os30 casos de
intervalos passaram também com ASan+UBSan e sync validation, sem erros Vulkan.
As regressões de TexMan/multirange, traps, sampled-layout/aliases, framebuffer,
draw textures, submissions, null descriptors, queries, conditional rendering e
TIC/cache passaram. A suíte TIC executou1145 verificações e52 checks de cache;
a variante lifetime executou53 checks e10 pipelines nativos retidos com fence
pendente, zero erros Vulkan. São medidas host, não FPS/frametime do Android.

A primeira tentativa TIC encontrou submódulo ausente. Foi inicializado exatamente
o pin`cb0ea66d573b3b58415b51f8dac0f18d82557b01`, sem alterar o gitlink ou os testes
TIC; as reexecuções passaram. Leak detection permanece desativada pelo problema
LeakSanitizer/ptrace do host; isso não certifica ausência de leaks.

A revisão independente conferiu todos os40 eventos e seus intervalos. Corrigiu
um offset da fixture do maior span (`0xcb0000`, demonstrado por assert vermelho
antes da correção) e exigiu comparação explícita do mip1 promovido com o antigo
backing do doador pendente; ambas as verificações integram a execução final.
`git diff --check` passou. Nenhum arquivo de produção/Android foi modificado.
