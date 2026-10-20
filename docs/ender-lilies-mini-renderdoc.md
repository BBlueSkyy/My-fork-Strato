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
