# Ender Lilies — mini-RenderDoc de cenário (Draft, diagnóstico)

Instrumentação **manual, limitada e sem correção gráfica** para investigar o cenário preto/branco com padrão de grade e recortes. O vídeo do usuário mostra diálogos e interface ainda desenhados, mas o cenário aparentemente corrompido. Isso não prova se a primeira divergência ocorre no shader, na textura, no render target, em blending, depth/stencil, sincronização ou apresentação.

## Base e escopo

Esta branch é **empilhada sobre** `debug/naruto-connections-mini-renderdoc` (#360), reutilizando os mecanismos já existentes de readback GPU, extração de shaders, CBUF, descritores de texturas/TIC/TSC, draw/vertex/index, Fermi2D/DMA e apresentação. A #360 original **não é modificada**. Esta instrumentação é experimental e **não deve ser mesclada** no master. O APK do diagnóstico é **DevRelease**. Essa variante compila a instrumentação C++ de captura, mas não inclui as camadas de validação Vulkan do tipo Reldebug. A captura continua opt-in.

A captura de Ender Lilies usa `gpu_capture/ender_lilies` e seleciona candidatos por dimensões/formato/uso de imagens, **sem Title ID, shader hash, nomes de driver ou hacks no jogo**. Os antigos símbolos internos `NarutoUiCapture`, `UI_PRE_*` e `TextFocus` são nomes herdados da infraestrutura, não implicam seleção por jogo.

## Como capturar no Moto G100

1. Instale o APK **DevRelease da PR deste diagnóstico**. Reproduza a corrupção em uma cena estável de Ender Lilies; não capture durante o splash screen.
2. Na pasta pública de arquivos do Strato, abra `gpu_capture/ender_lilies/`.
3. Renomeie `ARM_CAPTURE.rename_this_file_when_ready` para `ARM_CAPTURE` (sem extensão). Se a pasta ainda não aparecer, abra o jogo ao menos uma vez com a build do diagnóstico.
4. Volte ao jogo. O marcador é detectado quando um novo frame é apresentado; são coletados **24 presentation sources consecutivos**, e, nos primeiros **quatro intervalos aproximados**, até dois draws de cena classificados: primeiro com **blending ativo** e primeiro **opaco**.
5. Aguarde `capture_complete.txt` **e** as mensagens de readback `MINIRD_ENDER` / `MINIRD_CONNECTIONS` no `emulation.log`, porque algumas cópias de GPU podem terminar depois do marcador.
6. Compacte a pasta `capture_###` em ZIP. Envie esse ZIP, o **emulation.log** da mesma execução e um vídeo/print no momento da captura. Se possível, faça a primeira captura com o driver em que o problema se reproduz mais facilmente; use outro driver numa segunda execução somente para comparar.

**Não é preciso extrair o NSP ou modificar o jogo.** Não deixe o arquivo `ARM_CAPTURE` ativado continuamente; o processo remove o marcador após armar a sessão.

## O que conferir no ZIP

- `present_source_XX_*x*_*.raw` e `present_XX.txt`: imagens da fonte antes da cópia final para o swapchain, dimensões/formato/layout, frame ID e mapeamento do guest. Compare os pixels com o vídeo. Os arquivos são **RAW binários**, não PNGs.
- `gpu_events.txt`: eventos limitados por orçamento, shaders, draw target, sampled descriptors, transferências Fermi2D/DMA, indicações de backing e atualizações; `present_index_approx` **não é identificação exata de um draw por apresentação**.
- `pre_*scene_target_before*` (no metadata `role=scene_target_before`), `pre_*scene_source_before*` e `pass_*scene_target_after*`: trio de snapshots ordenados no mesmo command stream. Correlacione por `draw_id`, `submission`, `executor`, identidade de textura e mapeamento. Os nomes reais das imagens RAW são `pre_XX_rp...raw` e `pass_XX_rp...raw`.
- `SCENE_CONSUMER_DRAW`, `SCENE_CONSUMER_PRE`, `SCENE_FOCUS`, `PHASE2_SNAPSHOT`: candidate draws opacos/com blending e seleção de readback. A infraestrutura legada pode também produzir `UI_PRE_SHADER`, `UI_ACTUAL_CBUF`, `UI_ACTUAL_SAMPLER`, `ui_*shader*.spv`, binários de CB/VBO/índices, atlas e Fermi2D.
- Compreenda primeiro **qual é o primeiro recurso divergente**. Se a origem já está errada, examine as escritas anteriores, GMMU/aliases, decompressão e sincronização; se origem/target antes estão corretos e o target após está errado, examine shader, descritores, sampler, blend e depth/stencil; se target correto e apresentação incorreta, examine Fermi2D/VI/sincronização.

## Limitações e orçamento

Até 24 imagens de apresentação; no máximo 24 snapshots de source/target intermediários em quatro intervalos, total de **160 MiB**; no máximo 6000 eventos no trace; outros probes herdados da #360 têm limites próprios. Os dois primeiros draws qualificados por intervalo **podem não ser os responsáveis** pelo cenário defeituoso. Se isso ocorrer, use o histórico e o shader/target do trace para preparar uma segunda captura dirigida, em vez de alterar o renderizador por palpite.

Os readbacks e os splits de render pass são **opt-in** e podem afetar tempo, FPS ou esconder bugs de corrida. Não interprete ausência do bug durante captura como prova de correção. Esta branch apenas diagnostica, não tem novas esperas globais na execução normal. Não mesclar nem acompanhar Actions automaticamente; caso a build falhe, inspecionar o log fornecido pelo usuário.

## Segunda captura dirigida — diagnóstico confirmado em 19/10/2026

A primeira captura `capture_001.zip` (Turnip 24.0.99 / Adreno 650) contém **24 de 24** fontes de apresentação RGBA8 a 1280×720 já corrompidas, **antes da cópia ao swapchain Android**. Os alvos HDR `1280×720 B10G11R11UfloatPack32` e `640×360 B10G11R11UfloatPack32` exibem o cenário normal quando os dados são decodificados. Portanto, a hipótese prioritária passa a ser a transformação posterior, não a geração inicial da cena. Há também avisos de GPU-dirty buffer reads e Shader Compiler STUBBED, mas nenhum está demonstrado como causa.

A cadeia observada, a partir dos bindings reais dos `SCENE_CONSUMER_DRAW`, é:

1. Render target HDR `1280×720 B10G11R11UfloatPack32` -> draw de downsampling `640×360` (shader fragment `273A4DB6B12EC0A7`).
2. Pass de composição para `1280×720 B8G8R8A8Unorm` (vertex `ABE5AF9184BEC59C` / fragment `FB8C54CC98E5C241`), que lê HDR e outros efeitos, incluindo `320×180 B10G11R11`.
3. Pass final para `1280×720 R8G8B8A8Unorm` (vertex `53ADFB7B6E62953C` / fragment `A0E0DABE2B4B3F4B`), que lê o `1280×720 BGRA8` e **duas entradas `256×256 R8Unorm`**. O alvo RGBA8 corresponde a um dos três guest backings apresentados.
4. Um draw adicional com blending lê textura `1×1 RGBA8` e grava o mesmo alvo apresentado; sua responsabilidade no defeito não está comprovada.

**Nova seleção de captura:** os dois primeiros draws opacos que, por formatos e dimensões, casam com as transições HDR -> BGRA8 e BGRA8 -> RGBA8. Esses dois draws passam a usar as quotas herdadas (um de cada classe por intervalo) e tiram snapshot do source pré-draw, target pré-draw e target pós-draw, mais shader/CBUF/TSC/índices/VBO. **Não usa os hashes, endereços, Title ID ou driver para decidir a seleção.**

**Como reproduzir:** instale novo **DevRelease** desta PR, permaneça no mesmo cenário e repita o procedimento normal com novo `capture_###.zip`, log da mesma sessão. Não assuma que os snapshots de intervalos aproximados correspondem ao mesmo frame VI. Compare a saída do primeiro pass com a entrada do seguinte (identidade e pixels), em seguida a saída RGBA8 com o `present_source`. Se a saída BGRA8 já estiver errada, inspecione o primeiro fragment shader e seus demais inputs; se BGRA8 estiver correta mas RGBA8 estiver errada, inspecione o segundo shader, samplers, R8 LUTs, descritores e parâmetros de CBUF. Nenhuma correção funcional está incluída e não foi validada a compilação.

## Terceira captura: localizar o input culpado do primeiro shader (nova revisão)

A segunda captura completa provou no **mesmo submission 2862**: `pre_01` (1280×720 `B10G11R11`) contém o cenário; `pass_02` (`BGRA8`) já contém a silhueta negra e fundo claro; `pre_04` é o mesmo BGRA8 corrompido antes de ser consumido pelo pass final. `pass_05` mantém a corrupção. Esse resultado se repete no submission 2864. O `depth attachment` do draw HDR->BGRA é `none`, logo depth/stencil **não pode estar mascarando diretamente o seu output**.

O shader fragment `FB8C54CC98E5C241` tem **quatro operações** `OpImageSampleImplicitLod` no SPIR-V e o draw realmente contém quatro entradas (registradas em `SCENE_CONSUMER_DRAW bindings`): slot 0 = `32×32 A2B10G10R10UnormPack32`, slot 1 = `1×1 RGBA8`, slot 2 = `320×180 B10G11R11`, slot 3 = `1280×720 B10G11R11`. A anterior só guardou o slot 3 (correto), **não** os três primeiros. O BGRA8 capturado tem alpha=0 em todos os pixels, mas isso não prova sozinho que alpha cause o problema.

**Nova instrumentação (commit `b943f24`)**: usar as seis cópias por intervalo para `target_before` + **todos os quatro sampled inputs pré-draw**, incluindo slots 0, 1 e 2, + `target_after` para o primeiro pass **HDR->BGRA8**. A posição real dos slots vai em `SCENE_SAMPLED_INPUT slot=N`; cada source RAW é correlacionável por `PHASE2_SNAPSHOT`, `draw_id`, textura, formato e ordem dos eventos. O pass final continua rastreado em `SCENE_CONSUMER_DRAW`, mas não recebe mais cópias nem split de render pass.

**Por que não corrigir ainda:** a imagem HDR estar correta prova que o primeiro input principal foi produzido, mas não prova a qualidade dos 32×32, 1×1, 320×180, ou a tradução de amostragem/cálculos do shader. Identificar o primeiro input inválido ou uma divergência SPIR-V/CBUF é essencial para correção genérica sem regressões. A captura é manual, **DevRelease**, PR Draft sem merge; não acompanhar Actions automaticamente.
