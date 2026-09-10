# Animal Well e Constance: investigação de SetTerminateResult(674)

Estado após a terceira coleta: **Constance: exceção e operação que falha identificadas; Animal Well: aborto localizado no cliente de AudioRenderer. Os predicados que ligam esses caminhos a um defeito do emulador ainda precisam ser capturados. Nenhuma correção causal dos jogos confirmada.**

Atualização após teste do usuário: **defeito de lifetime das tabelas de símbolos do loader reproduzido e corrigido**. Esse defeito é do diagnóstico/stack trace do emulador; não é apresentado como causa dos abortos dos jogos.

Base desta branch: `92f2d08` da master, já contendo o PR #147. As outras branches e os working trees existentes foram preservados. Não se incorporou o scheduler do PR #146 nem o port de Mii do PR #148.

## Terceira coleta: evidência atual

APK testada: `e36900add9ed2ac2a154f75b880a627c1297f3ae`, build Android completa [34217297764](https://github.com/BBlueSkyy/My-fork-Strato/actions/runs/34217297764). A captura de pilha, instruções, payload e candidatos funciona nos dois jogos.

Identidade dos anexos atuais (SHA-256):

- `animal well.log`: `75661c262da74d4dcc1c79d9443578fb8bf6abacc321dfc7f42c4400d885dc9d`.
- `constance.log`: `4091704c339542774f8997e15cebb151116191be852370121a3d13a69a6a879a`.

### Constance

O erro concreto identificado no guest é **`std::__1::system_error`, código 11 (`EAGAIN`), com mensagem `clock_gettime(CLOCK_REALTIME) failed: Resource temporarily unavailable`**. Ele precede `std::terminate`, a notificação C++ e o resultado de aborto 674.

| Evidência | Valor |
| --- | --- |
| Primeiro Break (seq=4713, thread 1) | reason `0x80000007`, info `0`, size `0` |
| PC / LR do Break | `0x80922674C` / `0x8096D5B6C` |
| SP / FP / NZCV | `0x1989EFF0F0` / `0x1989EFF0F0` / `0x60000000` |
| X19 | `0x84FB98470`: string de nome demangled; **não** é o objeto da exceção |
| X20 | `0x809C13960`: RTTI, cujo nome em `0x809ABAC58` é `NSt3__112system_errorE` |
| Objeto da exceção, encontrado por X8 | `0x86FDF3D70`; vtable em +0, ponteiro da mensagem em +8, código 11 em +0x10, categoria em +0x18 |
| Mensagem | `0x84EDCEF08` (linhas 553–557 do log) |
| Chamada que verifica o relógio | `0x809679B0C`, argumento W0=0 (`CLOCK_REALTIME`), X1=SP (timespec) |
| Caminho de erro | branch por retorno não zero em `0x809679B10`; acesso a errno em `0x809679B4C`; chamada de lançamento de system_error em `0x809679B5C` |

O frame `0x809679B64` foi rotulado como `system_clock::to_time_t` porque LR coincide exatamente com o começo da função seguinte. A instrução anterior, `0x809679B60`, chama `std::terminate` a partir do caminho de erro de `system_clock::now`. A nova captura resolve retornos em **LR−4**, mantendo PC e todos os LRs brutos no log.

O Break observado ocorre no caminho de terminação da exceção. Não se confunde a notificação C++ com o lançamento inicial nem com a operação de relógio que falhou. Os 32 SVCs anteriores no histórico atual são QueryMemory bem-sucedidos do desenrolamento da exceção: eles não registram a primeira falha do relógio.

**Ainda não demonstrado:** qual Result/estado do serviço ou cliente de tempo produziu EAGAIN. A revisão do fork mostra inicialização de steady/local/network com o mesmo UUID e publicação dos contextos em shared memory; o log atual não contém essa memória nem o corpo de `clock_gettime`. Não se alterou esse contrato por hipótese. O erro de latência cubeb não é evidência de causa desta exceção.

### Animal Well

GetWorkBufferSize aceita REV15 e retorna Result 0 / `0xC3000`. CreateTransferMemory (seq=178) retorna Result 0 / handle `0xD01F`, source `0x802134000`, size `0xC3000`; CodeMutable é preservado, RW passa a None/Borrowed. PC `0x823B30580`, LR `0x823B026A0`, SP `0x19A57FF3F0`, FP `0x19A57FF410`.

A pilha desse retorno é `TransferMemoryImplByHorizon::Create` → `nn::os::CreateTransferMemory` → `nn::audio::OpenAudioRenderer` (LR `0x823BDBB98`) → overload com SystemEvent (LR `0x823BDBDE0`) → main. O próximo SVC é SetTerminateResult (seq=179). **O cliente SDK está dentro de OpenAudioRenderer; a implementação HLE do serviço ainda não foi chamada.**

O ponto de aborto em OpenAudioRenderer é a chamada em `0x823BDBD8C` (LR `0x823BDBD90`). O código/pilha apontam para a variante sem argumentos de AbortImpl. O frame `0x823A56ED0`, rotulado anteriormente como `AbortImpl(nn::Result const*)`, também está na fronteira com a função seguinte: a chamada em LR−4 pertence à variante sem argumentos. O Break fatal usa info `0x19A57FF42C`, size 4, e o valor real desses quatro bytes é **zero**. Isso não é uma intervenção do emulador.

**Ainda não demonstrado:** o predicado que desvia para esse aborto. O log tem código até `0x823BDBBD7` e volta em `0x823BDBD80`; falta o trecho intermediário após a criação da TransferMemory. Existe um branch para o mesmo aborto na parte anterior capturada, mas não é possível atribuir o caminho executado a esse branch. Não se conclui insuficiência de workbuffer, falha de alocação ou erro no handle só pela proximidade do aborto.

### Comparação e revisão preparada

Não há evidência de uma mesma falha estrutural nos dois jogos. Há caminhos concretamente diferentes: aborto do cliente de AudioRenderer e exceção de relógio da libc++. A falha comum já corrigida no loader afetava a leitura diagnóstica dos símbolos, sem demonstrar causalidade sobre 674.

A revisão seguinte é **instrumentação dirigida**, não correção comportamental dos jogos:

- Busca funções definidas em tabelas ELF próprias e validadas; captura OpenAudioRenderer na fronteira da TransferMemory e clock_gettime/relógios padrão na primeira notificação C++.
- Captura corpos limitados a 0x1000 bytes/função, destinos de B/BL, resolução de PLT por GOT e referências estáticas adjacentes ADRP+ADD/LDR. Limites globais: 48 corpos, 0x10000 bytes de código, 64 referências a dados, profundidade 3. Referências são candidatas estáticas, não uma trilha de branches executados.
- Preserva separadamente os 32 SVCs anteriores que não são QueryMemory, os 16 IPCs de tempo e os oito resultados IPC não zero, para impedir que o desenrolamento da exceção apague a evidência anterior.
- Registra os primeiros 0x200 bytes da shared memory de tempo ao exportar o handle e no Break da mesma thread, quando a referência continua válida, além da frequência do contador do host.
- Mantém payload, PC/LR, registradores, resultados e comportamento original de svcBreak/SetTerminateResult.

Validação adicional: `tests/kernel/diagnostic_snapshots.py` compila todo o diagnóstico de produção e a busca/resolução de símbolos com adaptadores de memória/logger Linux. Usa código AArch64 sintético para exercitar PLT relocada e referências a dados, símbolos indefinidos/inválidos, retenção do histórico após 500 QueryMemory, resolução LR−4, continuidade após falha de símbolos e preservação de memória/registradores/errno. Não usa binários dos jogos nem representa execução no aparelho.

**Próximo teste necessário:** executar os mesmos dois jogos na nova APK reldebug do PR #149, log Info ou Debug, e exportar os logs completos. No áudio, o corpo da função deve revelar os branches do trecho ausente; no relógio, os corpos e o estado compartilhado devem revelar a checagem que produz o erro. Se aparecer um defeito causal, sua correção deverá ser separada por causa e validada no aparelho. Não há dispositivo ou ExeFS dos jogos disponível nesta sessão para substituir esse teste.

## Primeira coleta: evidência dos anexos originais

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

## Procedimento das coletas anteriores (histórico)

Usar a APK **reldebug** desta branch com log em **Info ou Debug**. Executar Animal Well e Constance separadamente, preservando as mesmas versões/updates e configurações que geraram os anexos. Exportar um log completo de cada jogo após o aborto, incluindo o carregamento dos módulos.

Em Animal Well, correlacionar `GetWorkBufferSize`, `CreateTransferMemory RETURN`, qualquer QueryMemory e os frames de `SetTerminateResult`. Em Constance, começar pelo primeiro `svcBreak code=7`, não pelo término posterior. Usar PC/LR, frames, instruções e eventuais RTTI/mensagens para identificar o lançamento/checagem. Um frame sem símbolos ou uma leitura indisponível pode exigir um trecho adicional de ExeFS obtido da cópia do próprio usuário.

Somente após essa evidência criar correções comportamentais, separadas por causa. Uma correção comum exige comprovação de que os dois caminhos atingem a mesma falha.

## Segunda coleta: registradores confirmados e interrupção do diagnóstico

A primeira APK do PR #149 passou na build Android completa, execução `34177896386`, commit `f6ae420`. O usuário testou ambos os jogos e forneceu novos logs em Info.

**Animal Well:** REV15 `0x3F564552` aceita; workbuffer `0xC3000`, Result 0; CreateTransferMemory também Result 0, handle `0xD01F`. O owner mantém CodeMutable (tipo 4), passando de RW/atributos 0 para None/Borrowed. SVC 0x15 é seq=178, PC `0x823B30580`, LR `0x823B026A0`. A chamada seguinte, seq=179, é o IPC de SetTerminateResult; não houve outro SVC entre eles. O Break fatal posterior usa info `0x19A57FF42C`, size 4. O erro original contido nesses quatro bytes ainda não foi capturado.

**Constance:** o primeiro Break é seq=2140, reason `0x80000007`, info 0, size 0, PC `0x80922674C`, LR `0x8096D5B6C`, SP/FP `0x1989EFF0F0`. Candidatos preservados: X19=`0x86637A170`, X20=`0x809C13960`, X21=`0x8096D5C90`. X7=`0x726F7272655F6D65` contém os bytes ASCII `em_error`, compatíveis com o fim de `system_error`, mas insuficientes para confirmar o tipo/mensagem. O resultado 674 aparece na próxima chamada (seq=2141); o Break fatal vem em seq=2142. Não há base para atribuir essa exceção ao AudioRenderer.

Nos dois logs a coleta para com `Diagnostic read failed` depois dos registradores e antes da pilha. A inspeção encontrou `.symbols` e `.symbolStrings` armazenadas como spans de `Executable::ro.contents`, que é destruído ao retornar de LoadNso. GetStackTrace/ResolveSymbol consultavam essas referências pendentes. O teste `tests/kernel/loader_symbol_lifetime.py` usa a estrutura e o inicializador reais: após remover a .rodata temporária, a versão antiga falha com SIGSEGV; as cópias próprias da correção sobrevivem à remoção e à realocação do vetor de módulos. A resolução também exige terminador NUL dentro do tamanho de .dynstr, e as tabelas são validadas contra os limites de .rodata antes da cópia. Os dois produtores de `Executable` para NRO agora convertem offsets do arquivo para offsets relativos a .rodata, como exige essa estrutura; isso evita que a cópia leia fora do buffer nesses caminhos. Não se atribui esse defeito de offsets aos dois jogos.

A nova revisão imprime histórico e stack bytes antes de resolver símbolos, sempre imprime os endereços brutos, captura o payload do Break em etapa independente e mantém a inspeção dos ponteiros mesmo se a resolução simbólica falhar. Falhas informam a etapa e o PC/endereço do sinal. Isso evita que um erro auxiliar elimine o restante da evidência. Ainda é necessário repetir os dois jogos nesta revisão para localizar a checagem/lançamento no guest.
