# Armazenamento de 8/16 bits com fallback CAS

Base Strato: `650aae297c93510fd119dffb39d0e329602dc008` (`master`).
Checkpoints locais preservados: `a4277d5` e `f5f909b`.
Compilador publicado: `9cdd405f861229420a57f79832bf68b65c219fe6`, [PR Draft #16](https://github.com/BBlueSkyy/shader-compiler/pull/16). As árvores Git dos commits publicados são idênticas às dos checkpoints locais correspondentes.
Base do compilador: `cb0ea66d573b3b58415b51f8dac0f18d82557b01`, exatamente o gitlink desse master. A nova branch do compilador parte desse checkpoint; a PR usa `checkpoint/query-origin-recovered-20261115` como base para mostrar apenas esta adaptação, sem reapresentar as mudanças anteriores já fixadas pelo Strato.

## Auditoria e referências

- [Yuzu PR #12652, commit 2a0d707](https://ni.4a.si/anonymous/yuzu/commit/src?id=2a0d707ce1d4880dfcbd34c4d6572917a501f675): CAS U32 para preservar campos adjacentes nas escritas pequenas.
- [Eden issue #778](https://github.com/eden-emulator/Issue-Reports/issues/778) e [PR #4739](https://git.eden-emu.dev/eden-emu/eden/pulls/4739): a geração do helper deve usar a mesma condição do caminho que o chama.
- [Strato #464](https://github.com/BBlueSkyy/My-fork-Strato/pull/464): somente referência de VUIDs, ativação DevDebug e pinning da layer; nenhum commit da #463/#464 foi integrado.

O compilador fixado fazia U8/S8/U16/S16 stores nativos incondicionalmente. As leituras pequenas, as interfaces de SSBO e CBUF verificavam inteiros/aliasing, sem verificar armazenamento. Não existia fallback equivalente para essas escritas. Já havia CAS para outras operações atômicas; esses caminhos foram preservados.

## Implementação

- Perfil separa suporte aritmético de armazenamento SSBO e UBO de 8/16 bits. Emissão das interfaces, leituras, escritas e helpers usa os mesmos predicados.
- Fallback de escrita: atomic load U32 + bitfield insert + compare/exchange com escopo Device e ordenação relaxed, repetindo em caso de disputa. Valores com sinal gravam os mesmos bits baixos; leituras com sinal estendem o sinal.
- Antes de formar o ponteiro U32, cada helper compara o índice com `OpArrayLength` do SSBO. Índices fora do range retornam imediatamente, sem atomics. Descritores nulos e ranges com menos de uma word também não entram no CAS. Não há limite artificial de retries que possa perder escritas válidas.
- Como o Sirit fixado não expõe `OpArrayLength`, a etapa de assembly do backend insere o guard estruturado nos helpers, com IDs novos derivados do bound do módulo e checagens de integridade. Módulos sem esses helpers são retornados inalterados; nenhum submódulo adicional foi alterado.
- Um helper por binding realmente usado por uma escrita em fallback. Referência global ao SSBO evita nova dependência de `variablePointersStorageBuffer`. Shaders sem escritas pequenas em fallback não recebem helpers.
- Caminhos nativos recebem as capacidades/extensões SPIR-V corretas. Fallback não declara capacidades de armazenamento pequeno indisponíveis. CBUF direto e indireto também respeitam os recursos de armazenamento uniforme.
- Host consulta e habilita somente os bits suportados de `storageBuffer8BitAccess`, `uniformAndStorageBuffer8BitAccess`, `storageBuffer16BitAccess`, `uniformAndStorageBuffer16BitAccess`. `shaderInt8`/`shaderInt16` continuam independentes.
- Vulkan global permanece **1.1**. Armazenamento de 16 bits é core; 8 bits exige `VK_KHR_8bit_storage` habilitada. Sem a extensão, a estrutura de habilitação de 8 bits é removida, mesmo que o driver reporte API >= 1.2. Os testes também verificam recursos core em API 1.2.
- Novos caminhos UBO pequenos exigem o bit real de `uniformBufferStandardLayout`, sem confiar no aviso legado de suporte implícito.
- Alinhamento SSBO mínimo de quatro bytes garante base e cauda completas para operações U32, inclusive em hosts com mínimo Vulkan menor. O ajuste existente dos offsets usa o mesmo alinhamento; não se altera o formato de caches.

## Validação executada

1. **ASan + UBSan:** geração de 2.048 módulos pelo backend real, com todas as 128 combinações de recursos e versões SPIR-V 1.0/1.4. Binding dinâmico, índice excessivo e binding não declarado rejeitados antes da emissão.
2. **SPIRV-Tools do SDK 1.4.363.1:** todos os 2.048 módulos passam em Vulkan 1.1 ou Vulkan 1.1 + SPIR-V 1.4, com standard UBO layout. Verificação de capacidades e definição dos helpers em toda a matriz.
3. **Controle negativo:** substituição isolada do emissor de memória pelo original `cb0ea66` aborta no Sirit por tipo de ponteiro nativo indefinido quando há aritmética sem armazenamento; o emissor corrigido passa. Não representa uma execução integral histórica do Strato.
4. **Vulkan real, llvmpipe LLVM 19.1.7:** SDK/layer Khronos **1.4.363.1**, Synchronization Validation, aplicações API 1.1 e 1.2; **96 pipelines, 120 dispatches, zero erros de validação**. Perfis sem recursos pequenos, somente aritmética, nativo 8, nativo 16, totalmente nativo e aliasing desativado. Comparação de todos os 2.048 words por resultado, escritas concorrentes em bytes/halfwords distintos da mesma word, sentinelas adjacentes, extensão/truncamento de sinal, CBUF, múltiplos bindings e último byte válido. Preservados os 84 casos anteriores e acrescentados 36 de limites: OOB, índices extremos, null descriptor, range de três bytes, range de uma word, cauda truncada e offset de descritor não zero. Robustez de buffers desativada; fence com timeout de 30 segundos garante que um laço sem progresso falhe no teste.
5. **Regressão Warp32:** 262.144 casos; 102 módulos passam SPIR-V, 94 passam Vulkan; os 24 módulos em estágios suportados permanecem byte a byte iguais à referência do teste. Oito falhas de interface `Flat` já conhecidas e reconhecidas pela suíte permanecem (`06202`/`04744`), sem relação com armazenamento.
6. **Bindings SSBO existentes:** 96 caminhos reais passam, incluindo padding, ranges conhecidos/desconhecidos, mappings parciais e relocação.
7. Guardas do encadeamento de recursos, baseline Vulkan 1.1, extensão de 8 bits, alinhamento U32, isolamento DevDebug e pinning do APK passam. `git diff --check` passa nos dois repositórios.

SHA-256 do SDK Linux oficial utilizado: `b7867d8ca658563813af666f34b93db87f1b1c7d9d69d97e924c210c7ba33eb8`. A layer identifica versão 1.4.363; o sufixo `.1` é a revisão do pacote SDK.

## Integração de produção e teste no Moto G100

A PR #465 integra o fallback CAS com o compilador fixado em `9cdd405f861229420a57f79832bf68b65c219fe6` e mantém os testes de geração de shaders, recursos de armazenamento e guardas de SSBO.

Para a integração ao master, a instrumentação de diagnóstico da Vulkan Validation Layer foi retirada do caminho `CreateInstance`. O workflow da PR gera a APK **ARM64 DevRelease**, com verificação de integridade, assinatura Android e ausência de `libVkLayer_khronos_validation.so`. A layer pode continuar sendo usada nos testes Vulkan de host e nas PRs de diagnóstico separadas, mas não é incluída nem requisitada na APK de produção.

No Moto G100, usar o mesmo driver Turnip para comparar master e PR com fallback. A comparação visual do MK11 mostrou redução das listras e artefatos do chão, sem perda perceptível de FPS relatada pelo usuário. Isso é um resultado de campo; não demonstra que o `VK_ERROR_DEVICE_LOST` foi resolvido.

A regressão do crash continuará separadamente. O último log do MK11 mostrou um `vkCmdDispatch` com `groupCountX=4130713328` (limite reportado: `65535`), seguido de `VK_ERROR_DEVICE_LOST`; não há comprovação de ligação causal com o fallback.

## Limitações

- A comparação visual do MK11 no Moto G100 mostrou melhoria gráfica sem perda perceptível de FPS relatada pelo usuário. **Não se afirma correção de `VK_ERROR_DEVICE_LOST` nem ganho quantitativo de desempenho.**
- O guard CAS limita escritas ao range U32 real do descritor e descarta words inválidas antes de qualquer acesso atômico. Não define semântica Maxwell para acessos arbitrários nem corrige outros caminhos de memória. Os testes OOB executados usam fallback; o comportamento OOB do caminho nativo não é alterado por esta correção.
- O16 mantém a semântica existente de alinhamento para baixo a dois bytes; não há escrita partida entre words.
- Concorrência entre campos distintos de uma word é preservada pelo CAS. Operações não atômicas sobrepostas à mesma word continuam dependendo da sincronização do programa.
- A compilação Android e a validação de assinatura/arquitetura cabem ao PR Build DevRelease; os testes host mantêm a execução Vulkan com validação sem carregar a layer no app.
- O compilador permanece fixado no commit publicado da PR #16. As PRs #463, #464 e #466 são independentes desta integração.

## Correção da compilação Android

O run `44230551162` passou todos os gates host, incluindo os 2.048 módulos, e falhou no assembly Android por três conversões signed/unsigned em `EmitContext::Assemble` rejeitadas por `-Werror=conversion`. A contagem agora é explicitamente `size_t` e a cópia usa ponteiros dentro dos limites já verificados. Clang com `-Werror=conversion` passa; os 2.048 módulos regenerados com ASan/UBSan e SPIRV-Tools mantêm o hash agregado `35baa7a8803ba851e0205b680e6a80ea90cde3c5c28a264ee42182306aa7bb6e`, byte a byte iguais aos já validados. Os guards CAS e os recursos Vulkan permanecem idênticos. As builds DevDebug Validation anteriores servem como evidência histórica de diagnóstico. Para integração, a PR retorna a DevRelease limpa com os mesmos testes obrigatórios.
