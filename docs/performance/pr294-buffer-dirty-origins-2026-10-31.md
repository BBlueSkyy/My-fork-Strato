# PR404: origem dos uploads integrais — diagnóstico, sem sincronização parcial

> Errata interpretativa: origin0/1 identifica o callback roteado como escrita.
> NCE encaminha true sem distinguir o acesso real; isso não prova um store CPU.
> Ver pr294-buffer-cycle-order-2026-10-31.md para a coleta ESR/ordem desta etapa.

## Resultado e escopo

O usuário confirmou que 082dd3ed não recuperou FPS. O último log
`emulation(20261031-110834).log` (539 linhas) foi lido; confirma Zelda1.0.0,
Turnip24.0.99, E8B459126941F6CE/CBUF0/0x280 desconhecido em Phi e buffer143 de
8.470.528 bytes. Na última linha desse buffer há 653 uploads/5.531.254.784 bytes,
1,127474586 s de cópia CPU,271 trap waits/4,933567557 s e511 fast copies.
Esses são totais cumulativos, não janelas equivalentes à captura anterior.
Os ~197,3 MiB/s informados pelo usuário e a falta de ganho de FPS descartam
megabuffer como explicação suficiente. Nenhuma nova alegação de recuperação.

Base desta variante: a4001b73, com a coleta e experimento anteriores. Master
verificada: b8593d8b. Não é uma correção definitiva; mantém a PR Draft404 como
referência e a branch anterior intacta. Compiler97a97679 (c10731c2+diagnósticos)
inalterado: sem rollback, limites novos ou opções. Correção final será separada
na master mais recente, depois de comprovar causa e compatibilidade.

## Causa estrutural demonstrada / evento real ainda não isolado

- Construção staged começa CpuDirty, pois backing novo precisa de inicialização.
- Trap de escrita liberado muda Clean/CpuDirty para CpuDirty em todo o buffer.
- Trap após GpuDirty sincroniza guest e então muda para CpuDirty. Fast readback
  tem caminho separado e pode escolher CpuDirty/Clean conforme opção já existente.
- Primeira declaração GPU de escrita muda para GpuDirty; se o estado era CpuDirty,
  MarkGpuDirtyImplStaged chama SynchronizeHost(true) antes. Declarações repetidas
  enquanto GpuDirty não fazem uma nova transição.
- SynchronizeHost, tanto na preparação de GPU-write como no executor/coalescência
  e caminhos Write/Copy, só copia quando CpuDirty e copia mirror.size().
- SynchronizeGuest copia o mesmo tamanho quando GpuDirty e passa a Clean.
- Coalescência inicializa lacunas e preserva fontes/backings. O sucessor amplia
  a unidade de coerência; sua ligação é observada, não removida.

O teste extrai o corpo REAL de SynchronizeHost. Partindo de mirror/backing iguais,
um byte CPU é alterado, com CpuDirty: são copiados 8.470.528 bytes; todos os dados
são preservados. Segunda chamada Clean não copia nem espera. Isso reproduz
amplificação da arquitetura, mas não identifica a escrita que ocorre no aparelho.
As views GPU declaradas32KiB/2KiB não provam quantos bytes o shader escreveu.

## Por que não substituir por cópias parciais

`common/trap_manager.h` define TrapCallback como std::function<bool()>: o callback
não recebe endereço nem comprimento. HandleTrap conhece a página da falha, mas
chama writeCallback sem esse dado, muda a proteção da entrada para None e libera
os intervalos retornados por GetAlignedRecursiveRange. Escritas posteriores no
intervalo liberado podem não gerar outro callback antes de reproteger.
Mesmo conhecer somente a primeira página não prova cobertura completa das escritas.
Os WriteImplStaged/CopyFromImplStaged do host conhecem alguns spans, porém isso não
cobre os stores CPU que passam por traps. Não existe rastreamento completo para
uma implementação parcial segura nesta etapa. Aliases, GPU dirty, remapeamentos,
falhas/retries continuam usando os mecanismos existentes, sem atalhos.

## Coleta adicionada

SsboDirty agrega por buffer, origem, estado antes/depois, classe de intervalo,
offset/tamanho e ponto de chamada. As linhas são ligadas aos hashes/descritores
pelos bindings SsboTrace existentes e aos sucessores pela cadeia de coalescência.
Não exige IDs143/172. Somente buffers relacionados a Compute/ancestrais/sucessores
são reportados, até16 linhas adicionais a cada5s; sem log por draw/dispatch.

Origem numérica:0 TrapWrite,1 FastTrap,2 GpuDirty,3 CpuWrite,4 CpuCopy,5 Upload,
6 Download,7 GpuDeclared,8 Coalesce. Estados:0 Clean,1 CpuDirty,2 GpuDirty,
3 desconhecido/não amostrado. Classes extent:0 intervalo de modificação desconhecido,
1 span CPU memcpy efetivamente conhecido (mirror/backing/shadow conforme caminho),
2 view GPU declarada, NÃO bytes efetivamente escritos. Upload/download usam
size=mirror.size() como span candidato e copied como bytes de memcpy realizado.

Upload inclui chamadas sem trabalho (copied=0), chamadas reais e seu caller/linha
via source_location. Em cada cópia real há copy_ns e duração de WaitOnFence.
Essa duração pode incluir fence já concluída e não inclui toda a contenção de
mutexes, reproteção, flushes ou esperas de outros caminhos. As esperas no trap
continuam separadas nos contadores anteriores. Não somar tudo como tempo de GPU.
Retornos sem guest/direct e downloads sem trabalho não têm novas linhas de evento.

Os intervalos conhecidos são pares exatos offset/size, agregados por origem/estado/
callsite. A quantidade de linhas distintas NÃO é uma união disjunta de bytes e
não demonstra cobertura quando há entradas desconhecidas. Nenhuma escrita de trap
é convertida em intervalo inventado. A origem de GPU marca apenas a declaração.

Tabela fixa16.384 entradas,32 probes, sem novas alocações/locks no caminho quente;
contadores atômicos e publicação release/acquire. Há contador dropped explícito;
qualquer descarte limita as conclusões. As cópias mantêm spans, ordem e fences.
Contagem cumulativa pode ser amostrada em instantes diferentes pelo rodízio.
A revisão identificou colisões excessivas em offsets separados por páginas: teste
RED com33 regiões reproduziu perda prematura; mistura dos bits corrigida e teste
GREEN. Esse ajuste afeta somente a coleta nova, não a política de memória.

## Validação / próximo passo único

Passaram: corpo real SynchronizeHost (cópia integral/no-op/caller), agregação e
33 spans separados por páginas;1152 casos de binding SSBO e testes de megabuffer.
Bloco real do relatório compilado com fmt fixado pelo projeto. Revisão sem mudança
de decisões de coerência. Testes host não executam Vulkan/Android ou jogos.

Solicitar uma build ARM64 DevRelease desta coleta, sem acompanhar Actions.
Comparar logs de Zelda/Marvel com a configuração anterior, desde o lançamento e
90–120s de gameplay. Registrar FPS e gráficos completos; reiniciar o aplicativo
entre jogos. Analisar origem das transições CpuDirty e caller dos uploads reais,
separando GPU-write preparation de executor/coalescência e CPU Write/Copy. Se o
caminho dominante for trap desconhecido, não usar os spans host como cobertura.
Apenas depois decidir entre rastreamento completo por páginas/intervalos e uma
alternativa menor comprovada. Não publicar uma correção de uploads especulativa.
