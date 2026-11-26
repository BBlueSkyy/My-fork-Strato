# Auditoria funcional de DRAW_TEXTURE — PR #334

Esta implementação foi desenvolvida na branch `fix/maxwell3d-draw-texture`,
a partir da primeira tentativa `56e23a89e437a703c7528781aa1acdfd4e7d2e39`.
A versão inicial ainda exibia tela preta; a revisão funcional posterior
`203214bcd955278193db9ce1ffb2f39997c33620` foi validada no aparelho
com Steel Assault base (menu e gameplay). A PR #332 contém apenas diagnósticos
separados e não faz parte desta integração.

## Evidência e limite causal

A #332 comprovou 1.023 gatilhos do método final `0x42B` até QueueBuffer 1024,
com TIC8, TSC0 explícito e destinos correspondentes aos NvMaps 16/20/24.
As capturas anteriores comprovaram conteúdo válido no backing da fonte 800×480.
Esses dados identificam a operação de transporte. Não são uma captura de pixels
antes/depois do blit da #334, nem constituem quadros consecutivos de uma execução.

| Superfície | IOVA | Backing host | NvMap |
|---|---|---|---|
| Fonte 800×480 | `0x4009B0000` | `0x180ABE0000` | Não determinado para a fonte |
| Destino 1280×720 | `0x400000000` | `0x1809680000` | 16 |
| Destino 1280×720 | `0x400F60000` | `0x1809A60000` | 20 |
| Destino 1280×720 | `0x401330000` | `0x1809E40000` | 24 |

O master ignora esse side effect. A #334 já o executava; sua falha residual não
está atribuída exclusivamente ao sampler. O log anexado identificado como
MARVEL Cosmic Invasion não foi usado para inferir comportamento do Steel Assault.

## Divergências corrigidas

| Ponto | Antes | Correção e justificativa |
|---|---|---|
| TSC explícito | `GetSampler(ctx, 0, 8)` aplicava ViaHeaderBinding e escolhia TSC8 | Overload de lookup explícito escolhe TSC0. O overload de três argumentos continua aplicando a regra de shaders; tamanho do pool, bounds e cache são compartilhados. |
| Filtros reservados | Valores zero alcançavam o fim dos conversores sem retorno C++ | Retorno determinístico nearest com warning, seguindo a política de compatibilidade dos backends de referência. Não é uma afirmação sobre o resultado de descritores reservados no hardware. Filtros válidos e LOD não mudam. |
| Dimensões da view | UVs e destino usavam dimensões do backing inteiro | Usa dimensões do mip selecionado por `TextureView::range.baseMipLevel`, incluindo aliases que apontam a mip de uma imagem maior. |
| Vida da fonte | Fonte mantida no cache, sem dependência explícita no fence | Anexa a view fonte ao ciclo, como já ocorre para views nos blits Fermi2D. `AttachTexture` mantém o lock até a submissão; o ciclo mantém a dependência até conclusão. |
| Recorte | Helper usava scissor do tamanho inteiro da imagem | Intersecta retângulo, destino, SurfaceClip e scissor0; passa esse recorte ao scissor Vulkan. Mantém retângulos fonte/destino completos para não esticar UVs. |
| Origem Y | Bit 4 de WindowOrigin subtraía altura do retângulo | Remove essa translação. Ambos os emuladores usam o bit 0 de origem para DRAW_TEXTURE; bit 4 é flip de triângulos/shaders. Scissor inferior esquerdo usa `[H-yMax, H-yMin]`. |

O SurfaceClip segue a convenção já usada em `Maxwell3D::GetDrawScissor` do core.
A combinação inferior esquerda com SurfaceClip.y diferente de zero não tem
validação de hardware suficiente nas referências; não é apresentada como comprovada.
O caso observado tem origem do SurfaceClip zero.

## Caminho auditado

1. O engine armazena os registradores e dispara em `drawTexture.srcY`, após
   `CheckRenderEnable`. O layout do bloco ocupa 0x30 bytes; srcY está em 0x42B.
2. `Textures::GetTexture` resolve o TIC, formato/swizzle, view, mip/layer,
   tradução GMMU e `TextureManager::FindOrCreate`. TIC inválido, formato não
   convertido ou mapping ausente podem retornar uma textura nula de fallback,
   **não necessariamente um ponteiro nulo**. Metadados não provam seus pixels.
3. `GetColorRenderTargetForClear` atualiza o estado do CT selecionado e resolve
   sua view. Formato Disabled ou mappings inválidos retornam destino vazio.
   O caso observado tem CTSelect.count=1 e seleciona CT0. A semântica geral de
   múltiplos targets/nenhum target não foi ampliada nesta revisão.
4. O retângulo usa signed 20.12 e gradientes signed 32.32. Para os parâmetros
   observados, o destino é `(0,0)..(1280,720)` e a fonte percorre
   `(0,480)..(800,~0)`. Não há prova de retângulo vazio nesse caso.
5. Retornos restantes: fonte/sampler/destino indisponíveis, feedback da mesma
   imagem ou área vazia. Feedback mantém warning; nenhuma falha Vulkan é escondida.
6. Fonte/destino são anexados; destino recebe `MarkGpuDirty(UsageTracker)`.
   `AddSubpass` recebe uma sampled image e um color attachment. Alternar de RT
   para sampled separa render passes; a barreira existente AllGraphics inclui
   escrita de cor e leitura de fragmento. Não foram adicionados waits/barreiras.
7. O helper cria combined-image-sampler, push constants de origem/escala,
   viewport do destino, scissor recortado e `draw(6,1,0,0)`. Cull, descarte,
   depth/stencil e blend estão desativados; escrita RGBA habilitada.
   `NotifyPipelineChange` invalida o estado do pipeline programável seguinte.
8. Submissão sincroniza recursos e encadeia ciclos. A apresentação depende de
   encontrar a representação compatível do mesmo backing e de seus fences.
   Esta revisão não altera TextureManager, UsageTracker, NvMap ou apresentação.

O helper compartilhado conserva os defaults de sampler/scissor quando os novos
argumentos não são fornecidos; chamadas Fermi2D mantêm seu comportamento anterior.
Esta revisão não implementa toda combinação hardware de MSAA, formatos inteiros,
múltiplos attachments ou operações por fragmento. Não atribui esses limites ao
Steel Assault sem demonstrar seu uso.

## Comparação concreta

Referências locais auditadas em SHAs fixos:

- [Eden DrawManager](https://github.com/eden-emulator/mirror/blob/fecc1b4ee32bc06f736f2d138680ec948cf13ab1/src/video_core/engines/draw_manager.cpp):
  índice explícito TSC, conversão signed fixed-point e origem bit 0.
- [Eden rasterizer Vulkan](https://github.com/eden-emulator/mirror/blob/fecc1b4ee32bc06f736f2d138680ec948cf13ab1/src/video_core/renderer_vulkan/vk_rasterizer.cpp),
  `DrawTexture`, `GetScissorState`; `texture_cache/image_view_base.cpp` calcula
  tamanho pelo mip da view; `blit_image.cpp` emite descriptor, UVs e draw.
- [Ryujinx DrawManager](https://github.com/kotx/Ryujinx/blob/534f92506bf8c6aba29bb86917bf10954a360d8e/src/Ryujinx.Graphics.Gpu/Engine/Threed/DrawManager.cs),
  `TextureBindingsManager.GetTextureAndSampler`, `StateUpdater.UpdateScissorState`
  e `Vulkan/HelperShader.DrawTexture`: sampler direto, estado sem shaders,
  normalização pelo tamanho da view, inversão do scissor pelo bit 0.
- Filtros reservados: Eden `MaxwellToVK::Sampler` diagnostica e retorna o valor
  Vulkan zero (nearest); Ryujinx `SamplerDescriptor.ConvertFilter` e
  `Vulkan/EnumConversion` usam fallback nearest. São políticas dos emuladores,
  não especificações de comportamento reservado do GM20B.

## Validação e limites

`python3 tests/gpu_draw_texture/run.py`: 14 casos passam, com C++20,
`-Wall -Wextra -Werror` e UBSan. Os testes reproduziram falhas antes das correções.
Compilam o corpo de produção do DRAW_TEXTURE, seleção de sampler dos shaders,
conversores e layout dos registradores. As interfaces de GPU são substituídas.
Não compilam a construção real do sampler Vulkan nem gravam/executam a pipeline.
`git diff --check` passa. Android SDK/NDK não estão disponíveis localmente;
a compilação completa foi executada pelo GitHub Actions e passou no commit
`203214bcd955278193db9ce1ffb2f39997c33620`.

**Validação no aparelho:** o usuário confirmou que Steel Assault base, sem update,
renderiza corretamente no menu principal e durante a gameplay com esta correção.
A compilação e o funcionamento do jogo alvo estão confirmados; os 14 testes
host cobrem a semântica com interfaces simuladas, e não substituem verificações
GPU reais de outros títulos.

**Limite de regressão:** ainda é recomendável testar jogos com blits Fermi2D
ou samplers dependentes/independentes (por exemplo GRID, Nine Sols e Marvel).
Nenhuma instrumentação da PR #332 foi incorporada a esta implementação.
A PR também não pretende reproduzir todas as variações de DRAW_TEXTURE
e todos os modos de framebuffer sem evidência de uso.
