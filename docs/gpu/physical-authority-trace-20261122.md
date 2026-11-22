# Coleta diagnóstica de autoridade física — continuação de ec8e8b89

## Amostragem v2 após os dois boots do Moto G100

O relato do usuário registra OFF gravemente corrompido e ON quase correto, ambos
com a família RGBA16F 128×128/64×64 de seis camadas. Os 64 registros acabaram em
aproximadamente 3,14/1,19 ms, dominados por 16 writes, 16 draws e 17 attaches, sem
hashes, estatísticas HDR ou eventos de transferência. Esses números foram
fornecidos pelo usuário; nesta atualização não foram reanalisados arquivos brutos.
Eles demonstram saturação da telemetria, não a causa da corrupção.

A versão `sampling=authority-reserved-v2` mantém uma única família, oito imagens e
64 eventos, mas limita a atividade repetitiva por sessão: 4 write-intent, 2 draw-bound,
2 attach-use, 1 view-request, 2 submitted, 1 outro, 2 readback-request, 2 alias-select,
2 alias-reconcile. Assim, atividade comum e descrições usam no máximo 27 registros;
restam pelo menos 36 para eventos prioritários e um para o encerramento.

Readbacks efetivamente codificados/publicados, uploads, liberação/descarte e
mudanças observadas de dirty/freed/stale/retired/grupo/backing não entram nessas
cotas comuns. Mudanças vistas numa atividade já suprimida geram
`authority-transition`. Incrementos de serial isolados não geram um registro por
escrita. O estado observado e a associação ao ciclo continuam atualizados mesmo
quando a atividade não é emitida. Tentativas de readback/reconciliação sem cópia
são atividade limitada: não podem esgotar o orçamento reservado.

`activity_seen` contém contadores nas nove categorias acima; `suppressed` conta
atividade agregada, separadamente de `lost` por contenção. `critical_seen` conta
os eventos prioritários vistos. O marcador final pode ser `end`, com menos de 64
registros: isso é esperado e exige parar o jogo normalmente para obter o resumo.
A ausência de uma escrita individual no log amostrado não prova ausência de GPU write.
Notificações submitted também são amostradas; não se presume submissão de um
ciclo ausente. Os pontos de readback continuam observando apenas esperas já existentes.

O seletor não foi trocado arbitrariamente para outra imagem. Seis camadas são
compatíveis com um recurso de ambiente/cubemap, mas isso não comprova nem exclui
participação na iluminação do menu. O parser informa `no transfer evidence` quando
a família não alcança esses caminhos e mantém o vínculo ao menu como não comprovado
mesmo quando há transferências. Os dados antigos não permitem identificar o primeiro
recurso corrompido. Não se deve pedir vários boots idênticos se a nova coleta
continuar sem conteúdo: nesse caso o bloqueio passa a ser selecionar outra família
por uma ligação observada ao consumidor do menu, não aumentar logs globais.

Validação específica: simulação de 60.000 eventos repetitivos por sessão, com
seriais e submissões avançando, nos modos OFF e ON; transferências inseridas só
depois da sequência continuam capturadas, incluindo invalidação observada,
readback, upload e descarte. Cada sessão termina com 26 eventos, sem budget_end.
O limite duro de 64 continua testado com uma sequência de eventos prioritários.
São testes do coletor/decoder/parser, não execuções gráficas do jogo.

O próximo par de logs OFF/ON deve usar esta versão e conter o encerramento normal.
Ainda não se pode confirmar relevância da família nem causa-raiz no menu.

## Estado da investigação

Branch: `audit/physical-texture-authority-20261122`. Esta continuação implementa
somente observação. As três violações reduzidas permanecem documentadas no
checkpoint `ec8e8b891f516482db5b3e3053fba17057ecae2d`; seus testes não foram
reexecutados. A corrida da #480 não foi reaberta nem portada para esta build.

**Ainda não há uma violação comprovada no menu do SpongeBob.** Nenhuma execução
no Moto G100 ocorreu aqui. O bloqueio agora é receber os registros desta build,
com a classificação visual de cada boot. Os testes host da instrumentação não
substituem essa evidência.

## Seleção e limites

- O diagnóstico só é habilitado para o título `01009FB0172F4000` nesta APK.
- A família começa no primeiro overlap físico parcial, não representável como
  view de mip/layer, envolvendo uma textura guest HDR: Vulkan formato 122
  (B10G11R11 UFLOAT) ou 97 (RGBA16 SFLOAT). Esses formatos reutilizam os alvos
  relevantes dos relatórios anteriores; IDs locais de RenderDoc não são filtros
  estáveis entre boots.
- A primeira interseção física fica fixa. A família não cresce por overlaps em
  cadeia. São observadas no máximo **oito identidades de textura e 64 eventos**,
  incluindo o marcador final. Existe ainda uma linha de configuração por sessão.
- Cada descrição contém no máximo oito segmentos de mapeamento. Truncamento,
  limite de imagens, perda por contenção e fim do orçamento são explícitos.
- Os hashes FNV-1a abrangem até 4096 bytes da interseção no mirror já usado pela
  textura. Eles são amostras, não dumps nem prova de igualdade do recurso inteiro.
- Os dados HDR abrangem até 256 texels do prefixo do staging existente. NaNs e
  canais finitos acima de 1000 são contados separadamente. Esse prefixo linear
  não é necessariamente a faixa física da amostra Tegra, nem mede a imagem toda.

O seletor pode escolher uma família anterior ao menu ou sem relação com o
defeito. Nesse caso o resultado é inconclusivo; não se amplia a coleta
automaticamente. A assinatura de layout ajuda a escolher um filtro seguinte,
sem assumir que imagens semelhantes em boots diferentes são a mesma imagem.

## Eventos e evidência de execução

| Eventos | Significado e limite |
| --- | --- |
| family-seed, image | Descrição do layout, segmentos, identidade e interseção escolhida; histórico anterior à seleção não foi capturado |
| contained-unmatched | Candidato contido que não foi selecionado como view representável no matcher |
| fallback-candidate / fallback-readback-selected | Candidato considerado e fonte efetivamente enviada ao caminho de readback |
| cache-full-selected, view-request, attach-use | Seleção/reuso de textura ou view, incluindo descritores já mantidos pelo executor |
| alias-select / alias-reconcile | Participação nos mecanismos de aliases existentes; não cria dependências |
| write-intent / storage-write-unknown | Registro de uma intenção GPU e serial; não prova submissão nem cobertura dos pixels |
| draw-bound | Retângulo declarado do render pass e range da view; cobertura real do shader permanece desconhecida |
| clear-planned | Retângulo/range e bits do clear explícito; cobertura exata **se esse comando for executado** |
| cycle-associated / submitted | Associação da textura ao ciclo e notificação de submissão que já existia; sem polling de fences |
| readback-request / readback-guest-before / readback-guest-after / readback-published | Solicitação, hashes antes/depois da codificação e publicação; a entrada em CopyToGuest ocorre após as esperas já existentes |
| upload-source / upload-prepared | Bytes de origem e preparação do upload existente; preparação não é submissão |
| free-call / discard-result / free-flag-published | Chamada, limites reais e resultados de mprotect/madvise, e flag publicada pela implementação original |
| end / budget_end / no_family | Encerramento normal, limite alcançado ou ausência de uma família selecionada |

Os campos `a,b,c,d` dependem do evento: em regiões são x/y/width/height; em
discard-result, `a,b` são o endereço/tamanho realmente passado a madvise e `c` é
o mirror solicitado por FreeGuest. Endereços de mirrors e dos mapeamentos guest
podem diferir; igualdade virtual não é assumida como identidade física.
`result=-1` e `errno` registram falha de madvise, `result=1,b=0` significa ausência
de páginas interiores completas. `protection=-2` significa mprotect não observado
nesse caminho. O diagnóstico preserva inclusive flags de liberação possivelmente
inconsistentes: não as corrige.

`state_known=0` significa que os campos mutáveis não foram lidos com a garantia
de posse apropriada. Isso é intencional nos scans do manager e no fallback sem
lock de recurso. Um try-lock sem sucesso também produz estado desconhecido.
`group_known=0` impede atribuir os zeros a gerações reais de aliases.

Identidades de textura são únicas na sessão e distintas de ponteiros reutilizáveis.
Identidades de ciclos e grupos são apenas tokens locais; não se comparam seus
valores numéricos entre boots. Mudança de serial não prova autoridade para todos
os bytes de uma imagem parcialmente escrita.

## Preservação da execução

Não foram adicionadas cópias, submissões, barreiras, alocações GPU, Poll,
Wait/WaitSubmit, leitura de pixels por Mini-RenderDoc ou override de gpuDirty.
Os buffers de staging e mirrors observados já existiam nos caminhos originais.
O diagnóstico acrescenta leituras CPU limitadas e logging assíncrono; seu efeito
de timing/desempenho no aparelho ainda não foi medido.

Durante a coleta, locks diagnósticos são tentados sem espera. Contenção perde um
registro e incrementa `lost`; não bloqueia outra operação de textura. O estado é
observado apenas quando o chamador já detém o recurso e o try-lock de estado
permite. A coleta se desliga ao atingir seu orçamento. Não retém texturas,
imagens, cycles ou grupos de aliases para prolongar seu lifetime.

A opção CMake `STRATO_PHYSICAL_AUTHORITY_TRACE` é **OFF por padrão**. Hooks e
campos extras são condicionais; argumentos dos hooks não são avaliados quando
desativados. A build diagnóstica usa `-PphysicalAuthorityTrace=true`. A política
Free Guest, a API FreeMemory e suas decisões de sucesso/falha permanecem iguais.
Uma futura correção não deve levar esse diagnóstico habilitado.

## Validação e proveniência

O trabalho local recuperado incluía o coletor, adaptadores, analisador e resultados
host; não foi necessário reconstruir arquivos perdidos. A compilação de
integração que estava pendente terminou com sucesso. Restavam a documentação,
configuração da APK e publicação. Ajustes finais limitaram também o trabalho CPU
antes de selecionar a família e após atingir o orçamento.

Validação específica em `tests/gpu_physical_trace/run.py`:

- ASan/UBSan: faixa fixa, seleção HDR, identidades, até oito imagens/64 eventos,
  descarte, amostragem, decoder HDR, errno, notificações de ciclo e snapshots.
- Parser JSON e rejeição de sequência incompleta/reordenada; nenhum veredito
  causal automático a partir de hashes ou intenções de escrita.
- Compilação normal com argumentos diagnósticos não avaliados.
- Equivalência do código desativado em nove arquivos renderer/kernel com o
  checkpoint exato, normalizando somente dois scopes inertes de uma instrução.
- Ausência de novas operações GPU/esperas no coletor e opção OFF por padrão.
- Compilação dos corpos atuais de textura/manager/view/attach com os hooks ativos
  no harness existente, em modo **compile-only**. Nenhum dos testes antigos de
  coerência foi executado por essa etapa. Isso não é validação Vulkan da APK.

O CI valida a instrumentação antes de montar a APK ARM64 DevRelease, preserva os
gates já existentes e verifica que o binário contém os marcadores diagnósticos.
O resultado Android e os testes gráficos no Moto G100 devem ser registrados
separadamente; abrir a PR não comprova que uma APK foi produzida.

## Como coletar no Moto G100

1. Instalar somente a APK identificada como `arm64-physical-trace-release` desta
   branch/PR. Manter jogo, driver Turnip, cache e demais opções iguais. Não é
   necessário apagar shaders ou saves.
2. Fazer um boot com Free Guest **OFF**, aguardar o menu por alguns segundos,
   registrar se está correto/quase correto/corrompido, parar o jogo normalmente
   e exportar o log de emulação completo. Usar, por exemplo,
   `PHYS_OFF_corrompido.log`.
3. Fazer um boot separado com Free Guest **ON**, repetindo a mesma cena e
   classificação, e exportar `PHYS_ON_quase_correto.log` ou o nome que descreva
   o resultado real. Preservar o nível Info para as linhas `[GPU-PHYS]`.
4. Enviar inicialmente os dois logs e suas classificações visuais. Não é
   necessário repetir seis boots antes de conferir se o seletor capturou a
   família relevante. Se ambos ficarem iguais, isso também deve ser informado.

Não misturar as políticas forçadas das #478/#479. Dois boots com configurações
diferentes não isolam por si a intermitência: se o primeiro par for útil, a
comparação correta/corrompida deve incluir boots equivalentes da mesma opção.
Um encerramento abrupto pode perder linhas finais do logger; conservar o log
inteiro permite detectar essa limitação.

Análise local dos registros:

```sh
python3 tools/analyze_physical_authority.py PHYS_OFF_corrompido.log PHYS_ON_quase_correto.log
```

## Critério de conclusão e bloqueio atual

Os registros devem ligar uma fonte GPU efetivamente mais nova, sua região
escrita, a ordem de recuperação e o consumo dos mesmos bytes. Um clear executado
e uma cadeia de hashes podem fornecer uma prova mais forte que um draw com
cobertura desconhecida. Identificar apenas um overlap, NaN no prefixo ou hash
diferente entre boots não demonstra a causa.

O analisador registra candidatos e mudanças observadas; mantém
`confirmed_menu_violation=null`. Estado desconhecido, perda de eventos,
truncamento, família errada ou ausência de cobertura impedem uma conclusão
automática. O próximo passo depende dos logs reais. Não foi implementado
sequenciamento adicional, alteração Free Guest ou correção gráfica.
