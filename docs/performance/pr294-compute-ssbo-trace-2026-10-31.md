# PR #294 — identidade dos shaders Compute e custo dos buffers

Estado: **experimento de observação, sem correção de desempenho**. Base host:
`b8593d8b2c2e9742a4f1f9edfaa935f653cd3663`, master verificada em 31/10/2026.
Compiler: `97a97679a1d48877aec7d31c7871df28be239415`, baseado no gitlink da
master `c10731c2d5ab00dbc115e651edbb88e221dcf9cc`.
Referência da investigação: PR Draft #404. Nenhuma incorporação de #405/#406.

## O que já foi estabelecido

Segundo os testes no Moto G100 informados pelo usuário, `9da6fda2` (c7cb28d +
compiler/SSBO antigos) recupera Zelda e reintroduz magenta no Marvel. `d4ef6121`
(28bcb957 + compiler/SSBO novos) reproduz Zelda lento. O conjunto é causal;
a alteração específica dentro dele ainda não foi isolada. Não repetir esses testes.

Os logs da instrumentação `5a5d98e3`, fornecidos nesta etapa, foram lidos:

| Log | SHA-256 |
|---|---|
| Zelda link's awakening.log | b53e2e2e9bf35313c12f11e1f378b387ae7d534d8f4187e390798780760d6fc1 |
| marvel comics invasion.log | 4c8024d1e8638e4cce878e17dd36be35a9927875eac1e7537b10967a1ffe697c |

Últimas linhas disponíveis por chave Compute (contadores cumulativos; as chaves
são reportadas em rodízio e não têm necessariamente o mesmo instante final):

| Jogo / CBUF / offset | kind | calls | max request | max view | max backing |
|---|---:|---:|---:|---:|---:|
| Zelda / 0 / 0x280 | 3 | 3982 | 8388608 | 8388608 | 8470528 |
| Zelda / 0 / 0x290 | 3 | 2834 | 8388608 | 2089728 | 2101248 |
| Zelda / 0 / 0x280 | 2 | 3900 | 176 | 176 | 4096 |
| Zelda / 0 / 0x290 | 2 | 1066 | 208 | 208 | 2101248 |
| Zelda / 0 / 0x270 | 2 | 3900 | 196 | 196 | 2101248 |
| Zelda / 0 / 0x2A0 | 2 | 4067 | 4 | 4 | 8470528 |
| Marvel / 0 / 0x8 | 2 | 13592 | 196608 | 196608 | 233472 |
| Marvel / 0 / 0x10 | 2 | 13592 | 196608 | 196608 | 200704 |

Todos esses registros têm `direct_calls=0`. Os kind=3 suspeitos são somente
leitura; isso não impede que compartilhem buffers físicos com descritores que
escrevem. Os descritores sized escritos do Zelda em 0x310/0x320 também mostram
`max_backing=8470528`. Isso demonstra amplitudes iguais, **não identidade comum**:
o log antigo não possui ID físico, então não se pode afirmar que sejam o mesmo buffer.

Subtraindo os totais no início/fim da janela mais próxima de 60 s do final:
Zelda, 60,496787396 s, 12,374565 GiB de uploads normais; Marvel, 60,142354583 s,
0,782993 GiB. São todos os buffers. Não é prova de que os SSBOs causaram essas
cópias, nem uma comparação normalizada de cargas entre jogos diferentes.

## Lacuna e experimento único

O log antigo agrega por stage/CBUF/offset/kind/write, sem shader, pipeline,
dispatch, endereço de acesso ou buffer físico. É impossível recuperar essas
identidades a partir dos totais. Não existe bytecode dos shaders nos dois logs.
Nenhuma expressão concreta do Zelda pode ser apontada honestamente nesta etapa.

Esta branch mantém integralmente a classificação `has_size`, reconhecimento de
ponteiros de oito bytes, intervalos inferidos e fallback conservador de 8 MiB.
O compiler adiciona somente metadados de observação. O host os consome sem
alterar a geração de código ou decisões de mapeamento/sincronização. Não há
condições por jogo/offset, opções novas ou alteração de comandos Vulkan.

A coleta responde:

1. `SsboTrace pipeline`: hash do shader já usado pelo emulador, hash do estado
   PackedPipelineState (pipeline), dimensões locais e tamanhos de memória.
2. `descriptor`: CBUF/offset, sized/write, known, constante e coeficientes de
   grupo/local. Só os primeiros 128 pipelines Compute com ponteiros sem tamanho
   emitem esse bloco; `pipelines_dropped` informa excesso.
3. `inference`: primeiro acesso que torna desconhecido o intervalo daquele
   descritor, opcode de acesso, motivo, imediato de endereço e DAG SSA original,
   antes da substituição de memória global. Nós compartilhados/Phi não causam
   recursão infinita. Limites explícitos: 128 nós, 16 argumentos por nó;
   `dag_truncated` marca informação incompleta. Texto em partes de 1400 caracteres,
   com hash/pipeline/CBUF/offset/part/total em cada parte.
4. `binding`: identidade completa shader/pipeline/dispatch groups/CBUF/offset/
   kind/write/buffer. Distingue shaders e grupos que compartilham 0x280/0x290.
   Mede tamanho solicitado, view resultante, backing, mínimo/máximo de endereço
   guest, máximo offset da view no buffer e tempo CPU de Update/Attach.
   `mega_calls` conta bindings que retornaram um megabuffer.
5. `buffer`: ID monotônico do BufferManager, intervalo físico guest, direct,
   cópias normais host/guest com bytes efetivamente copiados e tempo CPU;
   esperas em WaitOnFence e no trap de escrita, separadas; marcas dirty/span;
   cópias efetivas para megabuffer e cópias CPU da coalescência com bytes/tempo.
   Fast readback, quando já habilitado pelo usuário, tem contador separado.
   A coleta não habilita nem depende dessas opções.
6. `successor_plus_one`: ligação do buffer antigo ao ID novo após coalescência,
   codificada como novo ID + 1; zero significa sem sucessor. Históricos de cópias
   anteriores ao primeiro binding são preservados. A apresentação resolve a
   relação com consumidores Compute em dois passes sobre os IDs (sucessores e
   ancestrais), incluindo custos de coalescência posterior, e reporta somente buffers relacionados
   a Compute. Um binding com ID UINT64_MAX significa view ausente/dummy.

`kind=0`: sized; `2`: raw Compute com accessSize calculável; `3`: raw Compute
sem accessSize. `known=true` pode ainda dar kind=3 por limite/overflow do GetSize;
os coeficientes e groups permitem distinguir isso de falha do TrackOffsetRange.

## Limitações da medição

Somatórios de request/view não são bytes copiados nem bytes acessados pela GPU.
Dirty span é amplitude marcada, não escrita realmente realizada pelo shader.
Cópias por buffer incluem **todos** os consumidores desse buffer, inclusive
aliases gráficos; identidade comum demonstra associação, não causalidade exclusiva.
Os ns são tempo CPU decorrido, incluem escalonamento/driver, não timestamps de GPU.
WaitOnFence pode encontrar fence já concluída. Upload/Attach/Update podem se
sobrepor conceitualmente (Attach/Update podem conter sincronização); não somar
todos os tempos como categorias mutuamente exclusivas. Fast readback e cópias
inline/direct/GPU fora dos pontos descritos não integram upload_bytes normal.

Tabelas fixas, sem alocação/locks novos no binding: 2048 chaves, até 32 probes;
32768 IDs físicos. Excesso explícito em `bindings_dropped`/`buffers_dropped`.
Hashes de pipeline são identidade diagnóstica, não uma prova matemática contra
colisão; o hash do shader e dimensões locais também são registrados.

Logs cumulativos e snapshots aproximados, no máximo 16 linhas de bindings +
16 de buffers + 1 total por 5 s. Textos de inferência saem na compilação do
pipeline, nunca por dispatch. A tabela fixa usa alguns MiB e a observação adiciona
leituras do relógio e atomics: esta build não serve para afirmar ganho de FPS.
O rodízio pode precisar de mais de 60 s para expor muitas identidades; cada total
mostra linhas e descartes. Os limites de diagnóstico não são limites de acesso.
Reiniciar o aplicativo entre jogos: IDs/contadores têm duração do processo.

## Validação executada

- Teste UBSan com IR real e funções de produção: mesmos resultados e fallbacks
  que c10731c2 para expressões conhecidas/desconhecidas, produto não affine,
  operador não suportado, compartilhamento, orçamento/profundidade; DAG com Phi
  cíclico e truncamento explícito. Esses exemplos são sintéticos, não shaders do Zelda.
- Contadores concorrentes, separação por hash/pipeline/dispatch/buffer/kind,
  overflow explícito, cópias anteriores ao binding, ancestralidade e coalescência.
- 1152 casos diferenciais do corpo completo GetStorageBufferBinding versus
  master: retorno, request, alinhamento, masks, dummy, dirty, barriers e megabuffer
  iguais, com UBSan. Não substitui validação Vulkan/Android.
- Bloco real de relatório compilado contra fmt fixado pelo repositório.
- Teste existente de framebuffer cache passou; código de framebuffer não mudou.
- Revisão independente: nenhuma mudança de decisão Vulkan/inferência encontrada;
  acrescentados contadores de cópias da coalescência e revisão de snapshots para
  registrar o último retorno de megabuffer.

Workflow existente adaptado somente nesta branch: ARM64 `assembleDevRelease`,
sem RelDebug. Verifica SHA host/parent/compiler, APK não vazio, biblioteca ARM64
no ZIP e checksum antes de upload. O estado do Actions/artefato deve ser consultado
na execução real; este documento não afirma sucesso de uma build ainda não concluída.

## Próximo teste no Moto G100

Instalar **SSBO Compute Trace — PR #294**, executar Zelda 1.0.0 na mesma região,
Turnip 24.0.99, resolução/opções iguais à master. Capturar desde o lançamento até
pelo menos 90–120 s de cena estável e sair normalmente. Repetir Marvel após
reiniciar o aplicativo. Conferir objetos, texturas, sombras, efeitos e interface;
registrar se Marvel segue sem magenta. Não é necessário limpar caches.

Nos logs, unir `inference`/`descriptor` com `binding` por shader/pipeline/CBUF/
offset; separar kind=2 de kind=3 e groups; unir buffer ID e ancestrais. Subtrair
snapshots do mesmo ID para medir cópias, dirty spans, wait e mapping em uma janela.
A partir da expressão real, só ampliar a inferência se houver prova conservadora
do limite, com teste de regressão no IR. Se o endereço for realmente indireto,
manter fallback e avaliar custo host sem reduzir memória acessível. Uma correção
só será PR Draft separada na master após evidência causal e teste dos dois jogos.
