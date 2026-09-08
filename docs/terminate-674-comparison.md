# Animal Well e Constance: investigação de SetTerminateResult(674)

Estado: **diagnóstico implementado; causas dos jogos ainda não identificadas; nenhuma correção causal confirmada**.

Atualização após teste do usuário: **defeito de lifetime das tabelas de símbolos do loader reproduzido e corrigido**. Esse defeito é do diagnóstico/stack trace do emulador; não é apresentado como causa dos abortos dos jogos.

Base desta branch: `92f2d08` da master, já contendo o PR #147. As outras branches e os working trees existentes foram preservados. Não se incorporou o scheduler do PR #146 nem o port de Mii do PR #148.

## Evidência dos anexos

| Caso | Sequência observada | O que falta |
| --- | --- | --- |
| Animal Well v1.0.10 | Linha 429: `GetWorkBufferSize` em 849497 µs; linha 430: `CreateTransferMemory` em 850000 µs, endereço `0x802134000`, tamanho `0xC3000`, handle `0xD01F`, permissão None; linha 431: entrada em `SetTerminateResult` em 850018 µs; linha 432: 674. | Revisão e parâmetros de áudio, resultado/tamanho devolvido ao cliente, PC/LR e caminho do guest entre criar TransferMemory e abortar. Não há `OpenAudioRenderer` no anexo. |
| Constance v1.0.3 | Linha 1510: `OpenAudioOut`; linha 1527: erro de consulta de latência cubeb; linha 1528: stream aberto com fallback de latência 480. Segue enviando/liberando buffers de AudioOut. Linha 11717: `Break(0x80000007)` em 2085168 µs; linha 11719: 674 em 2085204 µs. | Argumentos info/size, registradores, pilha e dados que identifiquem a exceção C++ e seu ponto de lançamento. Não há `GetWorkBufferSize`/`OpenAudioRenderer` nesse anexo. |

`674 = 0x2A2`: módulo 162, descrição 1 (`err::ApplicationAbort`). É o resultado de aborto informado pela aplicação; não identifica o subsistema nem substitui o primeiro erro. Referência: [Atmosphère err_results.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/err_results.hpp).

`0x80000007 = NotificationOnlyFlag | CppException`. A mensagem antiga “Debugger is being engaged” não identificava a razão 7. Ela não prova um assert específico nem que o debugger estava conectado. O retorno de uma notificação sem debugger é compatível com a implementação de referência; não é a falha que esta branch pretende corrigir. Referências: [libnx BreakReason](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h), [Mesosphere Break](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_exception.cpp).

O erro de latência do Constance é seguido de abertura do stream e operações de áudio por cerca de 0,78 s. O anexo não demonstra que esse fallback causou a exceção. A semelhança no resultado final não demonstra uma falha comum no renderer.

## Revisão de código

- `GetWorkBufferSize` propaga o resultado do audio-core e o tamanho calculado; a revisão aceita não é identificável a partir do anexo antigo.
- `CreateTransferMemory` registra sucesso na sequência observada e escreve Result em W0 e handle em W1. `WRegister` zera a metade superior do registro ao escrever um u32. Não foi constatado erro nessa atribuição.
- O owner de TransferMemory já preserva o tipo original e aplica a permissão solicitada e Borrowed; não se repetiu a correção anterior desse comportamento. O endereço de Animal Well está dentro de `.data + .bss` de main, não na Heap. A captura registra o tipo real recebido.
- O handle de transferência em `OpenAudioRenderer` não é consumido pelo shim como uma transferência Horizon completa. Há também limitações de validação de intervalos em KTransferMemory. Nenhuma delas foi ligada ao aborto anterior à entrada nesse método; não se alterou esse comportamento sem o caminho do guest.
- Há uma sobreposição preexistente dos offsets FPSR/FPCR com parte de Q31 em SaveCtx/LoadCtx, abordada por outro trabalho de scheduler/contexto. A captura desta branch preserva o comportamento atual. A presença desse defeito não comprova que ele cause qualquer um dos dois abortos.

## Captura adicionada

1. O trampoline salva X19–X30, SP original e seu LR interno antes de entrar no host. X0–X18 e NZCV vêm de SaveCtx. Todos os offsets TLS existentes são preservados. O PC do SVC é recuperado do branch de retorno do trampoline, não de um PC do host.
2. A cada SVC, um histórico limitado à thread conserva 32 chamadas completas, os seis argumentos e os dois primeiros registros de saída. Também são conservados os 16 últimos resultados IPC completos da mesma thread. Resultados não zero não são automaticamente classificados como erros: waits e chamadas opcionais podem devolvê-los legitimamente.
3. Em todo `svcBreak`, antes de qualquer retorno/encerramento, registra reason/code/NotificationOnly/info/size, PC/LR/SP/FP, X0–X30, NZCV, até 24 frames, bytes da pilha e instruções nos primeiros frames. Dados de info são limitados a 256 bytes.
4. Dados apontados por registradores são examinados com limites: até 48 endereços, 128 bytes/endereço, no máximo dois níveis de ponteiros. São candidatos não interpretados, úteis para RTTI/mensagens; não se chama `what()`, nenhum vtable do guest, nem se presume layout de `__cxa_exception`.
5. `SetTerminateResult` conserva o resultado verdadeiro e captura seu próprio contexto antes de responder ao IPC. A resposta de sucesso do serviço significa que ele recebeu o resultado, não que o resultado 674 foi transformado em zero.
6. A captura de áudio começa em `GetWorkBufferSize`: todos os parâmetros, resultado e tamanho; antes/depois de CreateTransferMemory: tipo/permissão/atributos, resultado e handle. Captura também QueryMemory subsequente e termina ao entrar em OpenAudioRenderer.

Leituras diagnósticas usam a descrição do VMM e `process_vm_readv` do próprio processo, com leitor opcional de `/proc/self/mem` se esse syscall estiver indisponível. Ambos são limitados ao próprio processo. Ponteiros ilegíveis, páginas removidas/protegidas, overflow e leituras parciais não são dereferenciados pelo host. Se ambos os leitores forem indisponíveis, isso aparece nos dumps. `errno` é preservado. Os dumps são síncronos para não ficarem perdidos no final da fila de logs.

## Validação e limites

- `git diff --check`.
- `tests/kernel/abort_diagnostics.py`: compila o emissor de trampoline e as definições de layout/instruções da produção; monta SaveCtx/LoadCtx e executa a sequência real de SVC em Unicorn AArch64. Verifica PC/LR/SP, X19–X30, saída W0/W1, retorno ao guest, TLS, stack e igualdade do estado final dos registradores com a base sem captura.
- `tests/kernel/diagnostic_memory.py`: compila o leitor de produção com um adaptador de metadados VMM e testa páginas Linux reais legíveis, PROT_NONE, removidas, leituras parciais, ponteiro nulo e overflow.
- `tests/kernel/loader_symbol_lifetime.py`: reproduz a falha das tabelas antigas após a destruição de .rodata e verifica que a cópia de produção sobrevive, inclusive à realocação dos módulos. Verifica também limites da cópia e conversão de offsets dos dois caminhos de NRO.
- Build Android local bloqueada antes da compilação: download do Gradle 8.2 falha por rede indisponível. A build Android completa deve ser verificada na CI do PR.
- Não há execução dos jogos ou acesso ao aparelho nesta sessão. Os logs fornecidos não permitem reconstruir retroativamente registradores ou o objeto da exceção. Não se inventa nome de assert, causa raiz nem jogo corrigido.
- Instrumentação temporária com custo de diagnóstico e possíveis efeitos de timing/layout. Deve ser removida após localizar as causas e não deve ser mesclada como correção definitiva.

## Próxima coleta

Usar a APK **reldebug** desta branch com log em **Info ou Debug**. Executar Animal Well e Constance separadamente, preservando as mesmas versões/updates e configurações que geraram os anexos. Exportar um log completo de cada jogo após o aborto, incluindo o carregamento dos módulos.

Em Animal Well, correlacionar `GetWorkBufferSize`, `CreateTransferMemory RETURN`, qualquer QueryMemory e os frames de `SetTerminateResult`. Em Constance, começar pelo primeiro `svcBreak code=7`, não pelo término posterior. Usar PC/LR, frames, instruções e eventuais RTTI/mensagens para identificar o lançamento/checagem. Um frame sem símbolos ou uma leitura indisponível pode exigir um trecho adicional de ExeFS obtido da cópia do próprio usuário.

Somente após essa evidência criar correções comportamentais, separadas por causa. Uma correção comum exige comprovação de que os dois caminhos atingem a mesma falha.

## Segunda coleta: registradores confirmados e interrupção do diagnóstico

A primeira APK do PR #149 passou na build Android completa, execução `34177896386`, commit `f6ae420`. O usuário testou ambos os jogos e forneceu novos logs em Info.

**Animal Well:** REV15 `0x3F564552` aceita; workbuffer `0xC3000`, Result 0; CreateTransferMemory também Result 0, handle `0xD01F`. O owner mantém CodeMutable (tipo 4), passando de RW/atributos 0 para None/Borrowed. SVC 0x15 é seq=178, PC `0x823B30580`, LR `0x823B026A0`. A chamada seguinte, seq=179, é o IPC de SetTerminateResult; não houve outro SVC entre eles. O Break fatal posterior usa info `0x19A57FF42C`, size 4. O erro original contido nesses quatro bytes ainda não foi capturado.

**Constance:** o primeiro Break é seq=2140, reason `0x80000007`, info 0, size 0, PC `0x80922674C`, LR `0x8096D5B6C`, SP/FP `0x1989EFF0F0`. Candidatos preservados: X19=`0x86637A170`, X20=`0x809C13960`, X21=`0x8096D5C90`. X7=`0x726F7272655F6D65` contém os bytes ASCII `em_error`, compatíveis com o fim de `system_error`, mas insuficientes para confirmar o tipo/mensagem. O resultado 674 aparece na próxima chamada (seq=2141); o Break fatal vem em seq=2142. Não há base para atribuir essa exceção ao AudioRenderer.

Nos dois logs a coleta para com `Diagnostic read failed` depois dos registradores e antes da pilha. A inspeção encontrou `.symbols` e `.symbolStrings` armazenadas como spans de `Executable::ro.contents`, que é destruído ao retornar de LoadNso. GetStackTrace/ResolveSymbol consultavam essas referências pendentes. O teste `tests/kernel/loader_symbol_lifetime.py` usa a estrutura e o inicializador reais: após remover a .rodata temporária, a versão antiga falha com SIGSEGV; as cópias próprias da correção sobrevivem à remoção e à realocação do vetor de módulos. A resolução também exige terminador NUL dentro do tamanho de .dynstr, e as tabelas são validadas contra os limites de .rodata antes da cópia. Os dois produtores de `Executable` para NRO agora convertem offsets do arquivo para offsets relativos a .rodata, como exige essa estrutura; isso evita que a cópia leia fora do buffer nesses caminhos. Não se atribui esse defeito de offsets aos dois jogos.

A nova revisão imprime histórico e stack bytes antes de resolver símbolos, sempre imprime os endereços brutos, captura o payload do Break em etapa independente e mantém a inspeção dos ponteiros mesmo se a resolução simbólica falhar. Falhas informam a etapa e o PC/endereço do sinal. Isso evita que um erro auxiliar elimine o restante da evidência. Ainda é necessário repetir os dois jogos nesta revisão para localizar a checagem/lançamento no guest.
