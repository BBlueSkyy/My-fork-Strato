# #481 — coleta exclusiva da expansão 128×128 RGBA16F, 1→8 mips

Continuação de `40e42308`, preservando os relatórios, logs e reprodutores anteriores.
Esta alteração é diagnóstica. Não corrige coerência, não muda Free Guest e não
adiciona cópias, esperas, polling ou comandos GPU.

## Auditoria específica

- CanGrowMipLevels rejeita memória liberada e exige layout nativo reversível,
  backing 2D ótimo, capacidade dos mips dentro do stride e do mirror existente.
  Não é autorização para considerar todos os bytes guest atuais.
- PromoteBackingForView preserva os níveis existentes por cópia da imagem GPU.
  Para os níveis novos, aloca o staging `initialization` e faz deswizzle do mirror
  guest por mip/camada. Essa origem é diferente do upload normal já instrumentado.
- RecordBackingPromotion recebe as regiões calculadas pelo caller. Transiciona
  imagens, copia níveis antigos GPU→GPU e registra buffer→imagem para os novos.
  Não resolve aliases nem procura um escritor-filho mais recente.
- TextureManager pode selecionar fullMatch e promover/reutilizar o pai antes do
  fallback de readback de matches. Quando o crescimento não é admissível, pode
  materializar outra representação. Esses caminhos serão observados, sem alterá-los.

A hipótese permanece: os novos níveis podem ser inicializados por guest antigo,
apesar de draws anteriores nos filhos. Os logs existentes não mostram os bytes
GPU dos filhos nem comprovam consumo desses níveis na iluminação do menu.

## Janela e orçamento

`sampling=mip-growth-v3` mantém oito imagens e 64 eventos, sem trocar de família.
Na família RGBA16F 128×128/seis camadas, atividade e transferências genéricas deixam
de emitir após 24 registros; estados/ciclos continuam sendo observados. O espaço
restante fica para `growth-*` e `consumer-*`. Descrições de imagens ainda podem
ocupar parte desse espaço, sempre dentro do limite total.

OFF pode manter o pai e os sete filhos. ON reserva o oitavo ID para uma possível
nova representação 128×128 com oito mips; a menor representação independente pode
ser omitida. Isso é amostragem, não invalidação ou mudança de lifetime. A omissão
fica sinalizada no indicador image_limit. O histórico anterior aos hooks e perdas
por contenção continuam sendo limites explícitos da coleta.

## Registros

- `growth-route`: primeira consulta 128×128/6 para oito níveis; a=solicitados,
  b=admissível, c=memoryFreed, d=níveis atuais. Rejeição com freed=true não prova
  que essa seja a única condição inválida.
- `growth-begin`: promoção 1→8 admitida e em preparação; ainda não é submissão.
- `growth-writer`: último estado observado de cada filho retido. detail[0] indica
  notificação de submissão do ciclo observado; detail[1] indica que existe uma
  amostra de readback já realizado, com endereço/tamanho/hash preservados no evento.
  Nenhum desses campos certifica conteúdo completo da imagem ou cobertura do draw.
  A ausência de amostra deixa o conteúdo GPU desconhecido. Não se adiciona readback.
- `growth-texel`: um texel RGBA16F em (0,0), nas seis camadas do mip 1 e na camada
  0 dos mips 2–7. a=endereço físico guest, b=offset lógico do mirror, c=offset no
  staging, d=token do buffer. view contém mip/camada. detail[0] são os oito bytes
  guest lidos antes do deswizzle existente; detail[1] são os oito bytes staging
  depois; detail[2] é igualdade. Não se compara hash de outro mip a esse texel.
- `growth-recorded`: caller está gravando as cópias existentes, a=níveis antigos,
  b=novos, d=buffer, detail[0/1]=imagens antiga/nova. context é o ciclo de promoção.
- `growth-submitted`: notificação existente do ciclo; não é conclusão GPU.
- `growth-published`: backing/níveis novos publicados no objeto Texture.
- `growth-alternate-created`: nova representação 128×128/6 com oito níveis,
  distinta do crescimento in-place. Permite observar a alternativa ON.
- `consumer-base-only` / `consumer-added-levels`: primeiro draw gráfico preparado com uma
  view base-only ou com uma view que permite níveis adicionais. a=count de níveis,
  view=mip/camadas, c=tipo da view; b indica hashes de shaders gráficos conhecidos.
  shaders contém os hashes Maxwell do pipeline, não uma identidade de SPIR-V
  especializado. destinations descreve até oito destinos sem acompanhá-los como
  novas imagens da família; formato e dimensões são os guest descritos. Não identifica
  consumidores compute. O evento é preparação do draw, não prova de execução.

No consumidor, detail[0] indica limites de sampler conhecidos; detail[1..3] são
bits float de minLod/maxLod/bias, detail[4] é o índice TSC resolvido, detail[5] o
mipFilter TSC, detail[6] marca ambiguidade se diferentes bindings foram observados,
e detail[7] é o índice TIC. São os últimos parâmetros observados para a imagem;
múltiplos bindings tornam a associação ambígua. Quick reuse não é reinterpretado
como um novo binding. LOD efetivo por fragmento permanece desconhecido: exige
examinar o shader e as coordenadas/derivadas, não apenas o clamp do sampler.

Os texels são amostras limitadas. Igualdade em doze texels não prova igualdade do
mip inteiro. Uma diferença pode refletir escrita CPU concorrente entre a amostra
e a cópia; não é automaticamente defeito do deswizzle. Tokens de buffers/imagens,
ciclos e hashes só são associados dentro da mesma sessão.

## Validação

Somente testes da instrumentação e compilação de integração, sem reexecutar a
auditoria #480 ou os três casos de ec8e8b89:

- ASan/UBSan: offsets mip/camada e bits correspondentes em doze texels; um
  staging deliberadamente alterado é identificado como diferente; limites do
  mirror impedem leitura parcial de um texel.
- Após 60 mil atividades repetitivas, eventos de crescimento/consumo posteriores
  ainda são retidos em OFF/ON. Limite duro, descarte de duplicatas por consumidor,
  rejeição ON marcada e slot para representação alternativa são verificados.
- Shader set, destinos, mip/view, LOD conhecido e escritor submetido com conteúdo
  GPU desconhecido são verificados pelo parser, sem conclusão causal automática.
- Código renderer/kernel desativado equivalente ao checkpoint de base, normalizando
  apenas scopes inertes da instrumentação; CMake continua OFF por padrão.
- Compilação dos corpos de textura/manager/views/attach com hooks ativos no
  harness, em modo compile-only. Não é validação gráfica ou de desempenho Android.

Evidência: `docs/checkpoints/mip-growth-trace-20261122/`.

## Teste no aparelho e critério de parada

Uma única APK ARM64 DevRelease diagnóstica será solicitada após a validação host.
Manter o mesmo jogo/driver/cache. Coletar um boot OFF e um ON, registrar o aspecto
do menu e parar normalmente para exportar o log. Usar a mesma APK em ambos.

As perguntas são finitas: OFF promove e de onde vêm os níveis novos? ON rejeita
a promoção ou cria outra cadeia? Os texels guest/staging correspondem? Há filhos
com intenção/submissão anterior e sem amostra GPU conhecida? Qual primeiro shader,
view/sampler e destino recebem o pai?

Se não houver consumidor capaz de acessar os novos mips no caminho do menu, a
expansão não sustenta essa hipótese para aquele boot. Se a observação não ligar
esse consumidor à corrupção, registrar o bloqueio e parar esta coleta; não
aumentar indefinidamente o orçamento. Se o conteúdo GPU dos filhos continuar
desconhecido por não existir readback no fluxo normal, isso permanecerá um limite,
e uma eventual cópia diagnóstica exigiria justificativa separada.

Nenhuma causa visual está confirmada antes desses registros no Moto G100.
