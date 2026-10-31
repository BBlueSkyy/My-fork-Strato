# Zelda: shader E8B459126941F6CE, buffer 172 e promoção indevida de cópias

Estado: **defeito da heurística de cópia reproduzido; variante experimental para
medir a contribuição aos FPS. Não é uma recuperação de desempenho comprovada.**
Investigação: PR Draft #404, sem merge.

## Fonte e identidade

Arquivo fornecido: `emulation.log`, 2045 linhas, SHA-256
`94f9d26e903546dfc51efd069831175abe300265ed3953c58413ac82df68a058`.
O log confirma Zelda: Link's Awakening 1.0.0, ID 01006BB00C6F0000,
Adreno 650 e Turnip 24.0.99. Contém a instrumentação SsboTrace de abf26faa;
não contém um identificador inequívoco do commit/APK.

O último relatório tem 4277 apresentações em 221,872784635 s desde a primeira;
`bindings_dropped=0`, `buffers_dropped=0`, `pipelines_dropped=0` e 2600 buffers
criados. Há 204 identidades de binding e 116 IDs físicos nas linhas reportadas.
O rodízio não garante um snapshot final simultâneo de todas as identidades.
Não há erro fatal registrado; os quatro ERROR são de latência do áudio (-4).
Isso não comprova renderização completa ou estabilidade fora do trecho.

## Shaders realmente identificados

| Shader / pipeline | Descritor | Resultado |
|---|---|---|
| E8B459126941F6CE / 94959D4E976EC4F2, local 1,1,1 | CBUF0 0x280, raw read | kind=3, LoadGlobal128, unsupported_opcode:Phi, DAG completo, request/view de 8 MiB |
| E8B459126941F6CE / 94959D4E976EC4F2 | CBUF0 0x2A0, raw read | kind=2, limite conhecido de 4 bytes |
| 2CB2DFE6C8B9A076 / 693C5EA5D8C7221D, local 32,1,1 | CBUF0 0x280 / 0x290, raw read | kind=2, limites 96 / 208 bytes |
| 1AC1E702BFC1B082 / 5532D6F700F64737, local 32,1,1 | CBUF0 0x290, raw read | kind=3, LoadGlobal128, unsupported_opcode:Phi, DAG truncado |

No E8..., o dispatch predominante registrado é groups=20,12,1; também há
16,9,1 e 18,11,1. A expressão de 0x280 contém Phi, contador iniciado em zero
com incremento de um, multiplicação por 0x90 e deslocamento de 0x80 antes de
somar o ponteiro. TrackOffsetRange não implementa Phi, então CollectAccessRange
retorna desconhecido e GetSize não fornece limite: o host usa o fallback de 8 MiB.
O DAG de endereços não inclui todos os predicados/limites do laço. Não se pode
provar um tamanho menor a partir dele. Nenhum limite foi inventado ou reduzido.
No 1AC..., há também bitcasts/cálculos float e o DAG é incompleto; esta variante
não tenta estender sua inferência. Compartilhar offsets não torna shaders iguais.

## Associação física agora demonstrada

O buffer físico **172** mede **8.470.528 bytes** e contém simultaneamente bindings
do E8...:

| CBUF0 offset | Papel declarado | max view | max offset no buffer |
|---|---|---:|---:|
| 0x280 | raw read desconhecido | 8388608 | 78592 |
| 0x2A0 | raw read conhecido | 4 | 1024 |
| 0x310 | sized escrito | 32640 | 34048 |
| 0x320 | sized escrito | 2040 | 68864 |

Os IDs anteriores 23 (8.396.800 bytes, só 0x280 entre os bindings Compute
reportados) e 25 (73.728 bytes, leituras/saídas pequenas) têm successor_plus_one=173,
isto é, ambos foram incorporados ao 172. O log não registra qual recurso/chamada
iniciou essa coalescência. Não atribuir sua criação exclusivamente ao 0x280.

As declarações de escrita são views pequenas em comparação com o backing.
`MarkGpuDirty()` insere o span **inteiro** de guest em dirtyIntervals; a primeira
transição staged para GpuDirty pode chamar SynchronizeHost se houver CpuDirty.
Chamadas repetidas enquanto já GpuDirty retornam cedo: duas declarações não
implicam duas cópias. `SynchronizeHost()` copia mirror.size() para backing;
`SynchronizeGuest()` também sincroniza no tamanho do buffer físico.
Um trap CPU trata a sujeira por buffer, não pelo tamanho do último descritor.

Portanto, o código e os contadores comprovam amplificação por granularidade do
backing. O log não fornece endereço/tamanho de cada escrita CPU nem contagem de
bytes realmente escritos pelo shader. Não é possível atribuir cada upload a uma
escrita pequena específica ou tratar toda a view declarada como escrita efetiva.

## Custos reais, não só tamanhos solicitados

No ID172, subtraindo as linhas **1655 e 2038**, timestamps 175,917685 e
236,490362 s: janela de **60,572677 s**.

| Operação do ID172 | Delta de chamadas | Bytes realmente copiados / span marcado | Tempo CPU registrado |
|---|---:|---:|---:|
| Upload staged normal | 1488 | 12604145664 (11,739 GiB) | 2,293022867 s |
| Push para megabuffer | 2527 | 21405024256 (19,935 GiB) | não medido |
| Fast readback existente | 1159 | 9817341952 (9,143 GiB) | não medido |
| Dirty marks | 1970 | 16686940160 (15,541 GiB), amplitude, não cópia | não medido |
| WaitOnFence | 768 | não se aplica | 0,371103826 s |
| Espera no trap | 734 | não se aplica | 12,209191504 s |
| Cópias de coalescência | 0 | 0 | 0 |

Cada upload e cada Push nesta janela tem exatamente os 8.470.528 bytes do
buffer inteiro. Os tempos são duração CPU decorrida e podem incluir driver,
escalonamento e espera de trabalho GPU de outros consumidores do fence. Não são
tempos de execução exclusivos do shader nem devem ser somados como custo de frame.
O uso de fast readback aparece de fato nesta captura; é separado de upload normal.
A alteração não habilita, desabilita ou depende dessas opções.

Antes da coalescência, o ID23 acumulou **10.454.016.000 bytes** adicionais de
megabuffer entre 19,640302 e 49,767881 s (~30,128 s), sem novos uploads normais,
sem dirty marks e sem downloads nessa janela. Isso demonstra um custo de cópia
adicional que não pode ser confundido com upload normal de dados modificados.
O ID1239 ligado ao outro shader/0x290 tem só um upload de 1.413.120 bytes e nenhum
Push/dirty mark/espera nas linhas disponíveis: não demonstrou o mesmo custo.

## Defeito específico isolado no host

Em `Buffer::TryMegaBufferView()`:

1. Uma view acima de 256 KiB é recusada para cópia por chunks.
2. Mesmo recusada, seu tamanho era somado a megaBufferViewAccumulatedSize.
3. A próxima chamada, inclusive uma leitura de 4 bytes, vê a soma maior que metade
   do backing e ativa a promoção para uma cópia integral do mirror.
4. unifiedMegaBufferEnabled persiste após ResetMegabufferState; novos ciclos ou
   sequências refazem snapshots inteiros. O contador confundia tentativa com cópia.

A função da referência boa **28bcb957 é idêntica à master b8593d8b**, hash SHA-256
do corpo `c59fde9257880a84f0f6d40572256f7502ee99df2bc8580368b189be71b136c4`.
Logo, a PR294 não introduziu essa heurística. O conjunto novo passa a expor views
raw de 8 MiB e encontra esse comportamento preexistente. Há evidência forte do
mecanismo de amplificação; sua contribuição causal aos FPS precisa do A/B no Moto.

## Alteração experimental mínima

Única mudança funcional: retirar `megaBufferViewAccumulatedSize += size` do ramo
que retorna rejeição. A promoção continua contando bytes de alocações/cópias reais.
A política existente de promoção legítima e seu caminho unificado permanecem.
Não se introduz um cap de binding: continuam acessíveis os mesmos 8 MiB e o host
pode retornar a view do backing normal pelo fallback existente, com
BlockSequencedCpuBackingWrites, locks, barriers, dirty tracking e fences intactos.

Mantidos compiler moderno, has_size, reconhecimento de ponteiros de 8 bytes,
validação da palavra alta, GetSize e descritores de storage images. O compiler
experimental 97a97679 contém somente diagnósticos sobre c10731c2; a variante não o
modifica. Os intervalos de ~192 KiB observados anteriormente no Marvel ficam abaixo
do limiar existente de 256 KiB; não entram no ramo alterado. Isso é um argumento
estático e um teste de mecanismo, **não uma confirmação de gráficos do Marvel**.

A sincronização de buffers inteiros continua: não alteramos a autoridade dos
intervalos ou copiamos regiões parciais sem rastreamento de escrita comprovado.
Menos megabuffering pode aumentar a frequência de uso do backing/fences; o ganho
líquido e a renderização precisam de teste. Esta variante não deve ser apresentada
como recuperação completa da regressão.

## Testes e comparação necessária

Teste RED no corpo real da função, antes da mudança: uma view rejeitada de 8 MiB
seguida de leitura de 4 bytes copiou **8.470.528 bytes** (assert esperado: 4).
Após remover a contagem rejeitada, copia **4 bytes** no fixture offset-zero.
Com os offsets pequenos observados e table shift de 13 bits, copia 772 e 1028 bytes,
sem promoção indevida. Este é um resultado sintético de bytes copiados, não FPS.

Passaram UBSan e ASan+UBSan (leak checking indisponível neste host): bytes dos
snapshots, reutilização, execução nova, AdvanceSequence na mesma execução,
promoção legítima (inclusive view grande após promoção), staged/direct/GPU-dirty,
e read inferido de 192 KiB. Passaram também os 1152 casos diferenciais existentes
de binding SSBO versus master. A revisão não encontrou bloqueador de correção;
o fixture não executa Vulkan real ou o comportamento de fences no aparelho.

Comparar **abf26faa** com esta variante experimental, mesma região/save de Zelda,
Turnip24.0.99, resolução/opções e estado térmico. Depois do mesmo trecho, enviar
log e observação de FPS/renderização completa. No log novo, IDs físicos podem mudar:
comparar por hash/descritor/ranges e não exigir ID172 de novo. Verificar bytes de
megabuffer, uploads, espera e conteúdo visual. Repetir Marvel e verificar magenta,
iluminação/efeitos/interface; preservar configuração entre as duas builds.

Se os Push grandes desaparecerem sem FPS melhor, o custo existia mas era
insuficiente para explicar a regressão. Se uploads/esperas continuarem grandes,
prosseguir na granularidade de sincronização ou limites reais do shader, sem
redução arbitrária. Correção final limpa na master e PR Draft separada somente
após essa validação. A branch atual mantém a instrumentação de isolamento.
