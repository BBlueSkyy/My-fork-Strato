# PR404 — ordem dos ciclos e acesso CPU real: diagnóstico, sem correção funcional

Base experimental:15c8da81 (master verificada b8593d8b + instrumentação anterior).
Compiler97a97679/c10731c2 moderno preservado, sem modificar has_size, ponteiros8B,
inferência ou fallback8MiB. Nenhum upload é eliminado, estado é corrigido ou
intervalo é reduzido nesta variante. Não é uma correção definitiva de FPS.

## Leitura dos dois logs

`zelda.log` foi lido integralmente (1647 linhas). O buffer143 mantém8.470.528B;
origens0/1/2 e uploads reais em MarkGpuDirtyImplStaged e SubmitInternal aparecem.
Os últimos contadores por chave saem em instantes diferentes: por exemplo,
FastTrap773 em89,79s, TrapWrite406 em99,86s, upload MarkGpuDirty454 em69,75s e
GpuDirty1426 em125,04s. As2521 chamadas de upload do executor reportadas em150,22s
estão na chave GpuDirty->GpuDirty com copied=0. Não são2521 transferências.
Não é possível ligar esses agregados um a um, nem dizer qual origem provocou
cada upload ou atribuir a ela o tempo de GPU. dropped=0 não resolve falta de ordem.
As janelas e cargas diferentes não são benchmark controlado de FPS.

`Marvel cosmic invasion.log` tem231 linhas e acaba em~6,59s, sem dados de gameplay
SsboDirty/SsboTrace suficientes. Não confirma compatibilidade ou ausência de magenta.

## Descoberta que muda a interpretação de origin0/1

NCE::SignalHandler e NCE::HostSignalHandler passam literalmente `true` para
TrapManager::TrapHandler. Jit32 também encaminha true. Isso já ocorre em28bcb957.
HandleTrap escolhe writeCallback por esse parâmetro; logo, origin=TrapWrite ou
FastTrap comprova o CALLBACK encaminhado, **não uma instrução CPU de escrita**.
A afirmação anterior de que o callback de escrita necessariamente prova um store
era forte demais. Um acesso de leitura pode ter sido classificado como escrita.
Ainda falta demonstrar que isso ocorre nas falhas relevantes do Zelda no aparelho.

Quando o acesso é de fato uma escrita, o fluxo FastTrap pode ser necessário:
backing->mirror antes de retornar do callback; com enableFastReadbackWrites=true,
o estado torna-se CpuDirty; a instrução que falhou pode então executar o store e
outras escritas no intervalo liberado podem seguir sem novos callbacks. Um upload
posterior preserva essas modificações. Com a opção false o comportamento legado é
Clean, com as perdas de precisão já inerentes à opção; esta variante não o modifica.
Trocar para Clean ou ignorar uploads como correção seria injustificado.

Quando o acesso é leitura e é encaminhado como write, o mesmo código pode baixar
e marcar CpuDirty mesmo sem store posterior. O teste local executa o corpo REAL do
callback fast e do upload com esse cenário, encontrando bytes idênticos reenviados.
Isso demonstra um mecanismo possível, não sua frequência real no Zelda. Nem todo
upload após FastTrap é redundante e nem uma igualdade isolada permite elidir cópias.

## Coleta direcionada nova

`SsboCycle` observa somente buffers físicos>=1MiB já relacionados a Compute.
O limite é de diagnóstico, sem mudar tamanho acessível/mapeado. Não fixa ID143/172
nem o jogo. Cada callback bem-sucedido incrementa uma época privada sob o stateMutex
existente. São amostradas épocas1,129,257...; não são uma amostra estatística
imparcial e podem não cobrir todas as origens. Cada upload captura sua época e
causa sob esse mutex, mantendo esses valores no registro final fora do mutex.

São2048 registros fixos e imutáveis, publicação release/acquire, sem espera de
produtor e sem alocação por registro. `order` é ordem de reserva do registro, não
ordem física absoluta entre threads: usar também time_ns e buffer/epoch. Descartes
são explícitos; o limite pode deixar o último ciclo incompleto. A apresentação emite
até24 linhas a cada5s, sem log síncrono no draw/dispatch. A configuração não muda.

| phase | Ponto observado |
|---:|---|
|0|callback normal permite acesso encaminhado como escrita, transição para CpuDirty|
|1/2|início/fim do fast readback antes de retornar ao acesso CPU|
|3/4|início/fim do upload real, com caller, bytes, copy_ns e wait_ns|
|5|transição para GpuDirty na preparação GPU, não escrita GPU já executada|
|6|memcpy CPU conhecido em WriteImplStaged/CopyFromImplStaged, com offset/size|
|7/8|início/fim de download normal no ciclo amostrado|
|9|entrada no caminho lento de callback antes de SynchronizeGuest|

cause0=callback normal,1=fast,99=inicial/sem origem atribuída. Flags:bit0 fast GPU
readback configurado,bit1 fast writes configurado,bit3 AllCpuBackingWritesBlocked,
bit4 parâmetro de acesso encaminhado ao callback. Bit2 não é usado: não se lê
shared_ptr cycle em caminhos que não possuem o lock externo necessário.

FaultScope TLS registra endereço da falha durante o callback; SignalScope NCE
registra PC e `actual_access`:0 leitura de dados,1 escrita de dados,2 desconhecido.
Esse valor vem do ESR no frame ARM64 para SIGSEGV de origem kernel; ausência,
registro malformado, outro tipo de exceção e cache maintenance ficam desconhecidos.
Jit32/outros chamadores continuam com classificação real desconhecida. O encaminhamento
legado true é mantido inclusive quando a observação diz leitura. Sem endereço/PC
validado no aparelho, não se identifica uma instrução específica do Zelda.

O parser é limitado ao __reserved do contexto, não segue ponteiros extra_context.
ABI verificada nos headers Linux/Android: ESR_MAGIC0x45535201, esr_context16B;
EC0x24/0x25, WnR bit6, CM bit8 excluído. Referências primárias:
https://android.googlesource.com/platform/bionic/+/3d4918e0d3e5b38ed00e7d048be266257137f90a/libc/kernel/uapi/asm-arm64/asm/sigcontext.h
https://android.googlesource.com/kernel/common/+/25cb20a212a1f989385dfe23230817e69c62bee5/arch/arm64/mm/fault.c
Scopes restauram o contexto TLS em retorno, retry e exceção C++; não alteram
callbacks, proteções de página, flags de roteamento ou o acesso CPU original.
O endereço é o ponto da falha, NÃO extensão de escrita; não cobre stores posteriores.

## Igualdade dos bytes e limites

Uma vez por época amostrada de upload, depois do WaitOnFence e da reproteção já
existentes, imediatamente antes do memcpy original, compara-se mirror/backing.
equal=0 diferentes,1 iguais,2 não comparados. compare_ns mede o custo extra, separado
da cópia e da espera. A cópia integral sempre ocorre. Não se compara o backing
adicionalmente dentro do fast callback potencialmente sem espera.

Igualdade é fotografia naquele ponto; não prova ausência de escrita CPU que gravou
o mesmo valor, nem estabilidade futura ou ausência de comandos GPU pendentes.
Diferenças podem incluir conteúdo GPU alterado depois do fast readback sem espera.
A época ordena observações do host, não execução de comandos na GPU ou todos os
stores guest. Callback/grant não é confirmação de que a instrução já completou.
phase3/4 fora do callback pode ter actual_access desconhecido/endereço zero: ligar
ao phase0/1/2 pelo buffer/epoch, sem interpretar esse zero como acesso ausente.

Spans de host conhecidos e declarações GPU continuam separados de escrita desconhecida
por trap. Não existe cobertura completa de modificações para cópias parciais.
A revisão removeu uma leitura nova de cycle sem seu lock. A tabela agregada Dirty
passa a contar publicação concorrente ocupada como descarte em vez de duplicar uma
chave atrás do slot em publicação, sem esperar no trap. Isso só afeta diagnósticos.

## Testes, próximos dados e publicação

Passaram testes com corpos reais do callback fast e SynchronizeHost: store após
callback preservado com fast writes=true; comportamento false permanece; upload
amostrado igual/diferente sempre copia; cenário de leitura encaminhada true reproduz
CpuDirty e reenvio sem store. Parser:read/write/unknown, contexto ausente/truncado/
malformado, CM, records anteriores ao ESR; scopes aninhados/exceção; publicação com
leitores/escritores concorrentes, overflow; testes anteriores1152 bindings/megabuffer.
ASan+UBSan no teste direcionado, leak checking desativado por restrição deste host.
São fixtures CPU: não executam Vulkan, ESR real do Moto ou gameplay. A compilação
Android será solicitada, não acompanhada, sem afirmar artefato produzido.

Para fechar a causa faltam ciclos completos do aparelho ligando actual_access,
PC/endereço, origem, configuração, igualdade e upload real ao shader. Se os ciclos
mostrarem leituras roteadas como escrita com uploads iguais, testar classificação
correta separadamente com casos de escrita/aliases e gameplay dos dois jogos antes
de publicar correção limpa na master. Se forem stores reais, manter coerência e
investigar custo de regiões/esperas com cobertura adequada. Não declarar recuperação
pelos bytes sozinhos. Coletar90–120s de gameplay de Zelda e Marvel, mesma configuração,
e confirmar FPS/renderização/magenta. PR404 preservada, nenhum merge ou solução final.
