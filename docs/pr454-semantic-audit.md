# Auditoria técnica e semântica da PR #454

Data: 13/11/2026 (Brasil). Base auditada: `5e38adee765e2c08b3d471377864e89969ad5b34`.
Compilador de shaders: submódulo fixado em `e972cae1d8de3c437769bb2b34fd23af7e030c40`.

**Conclusão: a PR evita o crash por uma política de compatibilidade, mas não comprova a semântica da Maxwell e ainda não é segura para merge.** O relato de gameplay do Kirby confirma utilidade prática da versão original. Não demonstra que um descritor reservado represente uma textura nula, nem que a causa anterior esteja corrigida. As alterações desta auditoria mantêm o caminho de amostragem com `nullDescriptor`, restringem dois comportamentos inseguros e deixam explícito o bloqueio de cache.

## Resultados por requisito

| Tema | Evidência e conclusão |
|---|---|
| TIC inválido Maxwell | O header público NVIDIA `clb197tex.h` define o campo de quatro bits e os alvos 0–8. O valor 15 em `TT_BIT_FIELD_SIZE` expressa a capacidade do campo, não um alvo de textura. Não foi encontrada especificação pública estabelecendo resultado zero para os alvos 9–15. |
| Ausência de GMMU | Não autoriza inferir um recurso nulo de hardware. Um endereço inválido pode pertencer a lixo, descritor não usado, leitura desatualizada ou corrupção. O fallback é uma escolha do emulador. |
| `Color2D` | É observável pelo compilador: `TexturePass` usa o tipo em queries de dimensões e para distinguir fetch 1D de texture buffer. Os testes reais do passe confirmam descritores de classes diferentes para `Color2D` e `Buffer`. O tipo codificado por muitas instruções de amostragem continua vindo da instrução; o fallback não converte automaticamente toda operação em 2D. |
| `HasResidentTicAddress` | Verificava somente o byte base: zero, limite de 40 bits, mapping não nulo e offset dentro do bloco. Isso é adequado para perguntar se a base está mapeada, não para validar a extensão de uma textura. Foi renomeado para `HasMappedTicBaseAddress`; o comentário de 48 bits foi corrigido. |
| Causa anterior | O log do crash às aproximadamente 11,866 s mostra `GetTextureType`/`TexturePass`, mas não contém índice, endereço nem os 32 bytes do TIC. Portanto não distingue descritor não inicializado, corrupção, sincronização ou leitura antiga. Nenhuma dessas hipóteses foi tratada como causa comprovada. |
| Outros emuladores | Ryujinx/Ryubing rejeita endereço zero/não mapeado antes de construir textura, e possui invalidação do estado inválido. Também usa fallback de tipo 2D, **com rastreamento de especialização**. Copiar somente o fallback não copia essa proteção de cache. É precedente de compatibilidade, não teste de Maxwell. |
| Regressões | Alvos válidos 0–8 mantêm sua classificação. Alvos reservados com base mapeada continuam fatais. Bases zero, fora de 40 bits, não mapeadas e sparse mantêm fallback de compilação. Amostragem reservada exige `nullDescriptor`; storage reservado é rejeitado. Texturas buffer com tipo já definido pela instrução usam caminhos distintos e não recebem uma promessa adicional de validação desta PR. |
| Testes executáveis | Há execução dos métodos de produção em fixtures, do passe real do compilador e de shaders SPIR-V em Vulkan com validação. O probe de especialização de cache continua vermelho; isso é registrado como falha, não convertido em um teste que aprova o comportamento defeituoso. |

## Problemas comprovados e corrigidos

1. **Storage com alvo reservado descartava escritas silenciosamente.** O mesmo `GetTexture` usado por sampled images devolvia `Unmapped`; `GetImageBinding` aceitava isso como motivo para binding nulo e descarte de stores. Um alvo reservado sequer fornece uma extensão de imagem válida. Foi introduzido `NullTextureReason::InvalidDescriptor`, rejeitado pela política de storage, inclusive quando o host suporta `nullDescriptor`. O caso anterior de um TIC decodificável com memória ausente mantém a política existente.
2. **Host sem `nullDescriptor` recebia o placeholder 2D compartilhado.** Ele não satisfaz necessariamente um shader cube/3D/array e sua criação não estabelece conteúdo zero. O novo caso reservado agora falha explicitamente nesses hosts. Com `nullDescriptor`, amostragem continua usando imagem nula e sampler real, como na PR testada no Kirby.
3. **Terminologia superestimava a validação.** A consulta de base não valida formato, header version, largura/altura, mips, camadas, footprint, permissões ou estabilidade futura do mapping. Não se tenta calcular extensão para um alvo reservado. Comentários que atribuíam semântica de acesso Maxwell ao comportamento Vulkan foram corrigidos.

Os testes novos foram escritos e executados antes da correção: 1.114 verificações com 505 falhas na PR original, incluindo storage e sampled sem suporte. Depois da correção, as 1.114 passaram com ASan/UBSan. Três controles adicionais de cache de views através da mudança de sequence number elevaram o total para **1.117 aprovadas**. Os resultados anteriores foram preservados.

## Bloqueio de cache de shaders/pipelines

Reprodutor: `python3 tests/gpu_tic_validation/run.py --probe-cache`.

1. Um TIC reservado/não mapeado é consultado e produz metadados `Color2D` na criação do pipeline.
2. O mesmo slot passa a conter um TIC válido `e1DBuffer` em memória mapeada.
3. A consulta atual retorna `Buffer`, mas a mesma chave de pipeline ainda retorna o objeto compilado com `Color2D`.

**Quatro asserções de correção falham:** mapa gráfico, retorno do pipeline atual, transition cache preenchido e mapa compute. O fixture compila as funções reais de lookup; a construção do pipeline é uma fachada que captura o metadado consultado. Isso comprova reutilização sem revalidar aquele metadado, não um erro de validação Vulkan ou falha visual observada no Kirby. O teste separado do compilador mostra por que essa diferença altera a classe do descritor.

A arquitetura já tinha esse problema antes da PR, inclusive para entradas fora do pool e outros valores lidos na compilação. O fallback novo expõe outro caminho para ele. Os mapas não são indexados pelos tipos de TIC consultados. `PipelineStateBundle` registra valores para persistência, mas a consulta runtime/transition não os revalida.

Não foi feita uma substituição de `unique_ptr<Pipeline>` no mapa: isso pode invalidar referências cruas mantidas por caches e comandos. Comparar só o TIC anteriormente consultado também é insuficiente: alterações no constant buffer podem selecionar outro índice. A solução correta precisa acompanhar os valores usados na compilação, cobrir pipelines atuais/transitions/compute/preloads e preservar objetos em uso. Recompilar indiscriminadamente por draw prejudicaria desempenho e foi evitado.

O workflow desta PR executa o probe como **gate obrigatório antes de montar o APK**. Enquanto ele falhar, não há APK elegível segundo o requisito de só gerar Release após todos os testes necessários passarem. Não há merge nem conversão para Ready.

## Investigação de leitura e sincronização

`TexturePoolState::Flush` obtém um span da memória CPU mapeada por `LookupBlock` e o interpreta como TICs. O tamanho do pool e a leitura não constituem validação semântica dos descritores. A consulta não materializa por si só escritas GPU pendentes sobre essa memória. O cache de views tem fast path por `channelSequenceNumber`, depois compara o TIC; I2M pitch e flush incrementam essa sequência. Os testes verificam que um cache válido não impede a classificação de um TIC alterado após essa fronteira.

Nada no log demonstra que o pool foi dividido entre mappings, que houve escrita GPU pendente sobre o TIC, que uma fronteira foi perdida ou que os bytes foram corrompidos. Não foram alteradas barreiras, leitura de pool ou shaders sem uma reprodução que justificasse isso. O diagnóstico existente de TIC inválido continua ativo; um dump do descritor e seu produtor ainda é necessário para fechar a causa específica do Kirby.

## Validação e limites

| Verificação | Resultado |
|---|---|
| TIC/bindings de produção em fixture, ASan/UBSan | 1.114/1.114 no checkpoint, 1.117/1.117 após ampliar controles |
| `TexturePass` real fixado, UBSan | 16 combinações bound/bindless × 8 alvos aprovadas; classes de descritor verificadas |
| Vulkan lavapipe + camada Khronos | 7 dimensionalidades sampled e 1 storage write/read; 64 palavras zero; 0 erros de validação; SPIR-V validado |
| `gpu_null_storage_image` | Aprovado |
| `gpu_sampled_layout` e aliases | Aprovados no checkpoint; avisos preexistentes de depreciação `shared_ptr` no host |
| `gpu_texman_multirange` | 3 executáveis aprovados no checkpoint |
| `gpu_submission` | Aprovado |
| `gpu_framebuffer_cache` | Aprovado com headers Vulkan correspondentes |
| `gpu_tic_validation --probe-cache` | **Falha: 4 asserções**, bloqueio semântico preservado |
| `gpu_backing_promotion` (Vulkan) | Falha `Cannot losslessly reconcile these texture alias layouts`; reproduzida também no HEAD original `5e38adee`, com Clang e headers Vulkan correspondentes |
| `gpu/run_pitch_linear_tests.sh` | Falha de compilação do fixture por membros de backing/layout ausentes; reproduzida também no HEAD original `5e38adee` |

Vulkan confirma garantias do host para recursos nulos; não mede a Maxwell do Switch. Os testes com fachadas não são boot de Android nem execução de Kirby. A preservação é do caminho de amostragem já usado pela PR, sem nova validação em aparelho nesta sessão. Não há alegação de ausência global de regressões.

## Referências verificadas

- [NVIDIA: classes e GPU t210/Maxwell B](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/README.txt).
- [NVIDIA: layout e tipos TIC](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197tex.h).
- [Ryubing #179: rejeição de descritores não mapeados](https://git.ryujinx.app/projects/Ryubing/pulls/179) — página protegida por Anubis; implementação conferida no código abaixo.
- [Ryujinx TexturePool](https://github.com/Stella-sea/ryujinx-admin/blob/canary/src/Ryujinx.Graphics.Gpu/Image/TexturePool.cs).
- [Ryujinx conversão de TextureTarget](https://github.com/Stella-sea/ryujinx-admin/blob/canary/src/Ryujinx.Graphics.Gpu/Image/TextureTarget.cs).
- [Ryujinx GpuAccessor](https://github.com/Stella-sea/ryujinx-admin/blob/canary/src/Ryujinx.Graphics.Gpu/Shader/GpuAccessor.cs) e [ShaderSpecializationState](https://github.com/Stella-sea/ryujinx-admin/blob/canary/src/Ryujinx.Graphics.Gpu/Shader/ShaderSpecializationState.cs).
- [Khronos: nullDescriptor](https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VkPhysicalDeviceRobustness2FeaturesEXT.html) e [especificação Vulkan](https://registry.khronos.org/vulkan/specs/latest-ratified/pdf/vkspec.pdf).
- Código local: `textures.cpp`, `pipeline.inc`, `common/address_space.h`, `pipeline_manager.cpp/.h`, `pipeline_state.cpp` e submódulo `ir_opt/texture_pass.cpp` no commit acima.

Revisão independente: não encontrou novo defeito no ajuste restrito e confirmou o bloqueio preexistente de especialização. Isso não é aprovação para merge.
