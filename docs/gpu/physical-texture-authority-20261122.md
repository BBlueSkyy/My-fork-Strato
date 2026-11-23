# SpongeBob: dependências entre representações físicas — continuação de 22/11/2026

## Resultado e alcance

A hipótese tem uma **violação concreta no fork**: duas imagens Vulkan diferentes
que compartilham memória física guest podem continuar sendo tratadas como fontes
independentemente válidas. Isso permite reutilizar uma imagem em cache antiga ou
enviar dados guest obsoletos para uma terceira imagem, mesmo depois de os
produtores GPU terem terminado. A relação desse defeito com os pixels corrompidos
do SpongeBob **ainda não foi demonstrada**.

Base conferida no GitHub: master `8bcaee0be950b44234cdd2767a11578c74db2602`.
Também executei os novos casos com os métodos de textura da #480,
`8453cc6f10500a7cf6a99bf14f85eaf66e50d489`: o resultado permanece. Não repeti os
testes da corrida corrigida naquela PR.

Free Guest OFF permite o consumo antigo no teste. **ON também permite**: no caso
de um overlap contido omitido da recuperação, o modelo de descarte troca o
sentinela antigo por zeros. Não há prova de que ON impeça esse caminho no jogo.
Isso é compatível com mascaramento, sem estabelecer que explique a melhora visual.

Não modifiquei produção, Free Guest ou qualquer PR anterior. Não fiz merge,
instrumentação de aparelho nem build Android. Esta branch preserva somente testes
de auditoria, resultados e este relatório; não é uma correção gráfica.

## Checkpoints reaproveitados

- #477–#479: a separação entre a opção e a política genérica gpuDirty já foi feita.
  Não há nova variante A/B. Os quatro logs amostrados e suas limitações foram
  analisados na #480; não identificam o primeiro recurso incorreto.
- #480: preservação da autoridade durante o readback. Os casos novos são
  serializados, sem CPU concorrente, e sobrevivem à aplicação daquela correção.
- #460, head `579955a318cd00a632e3000a21c4ca2252f05730`: li o mecanismo específico
  pendingGpuWriteTag / SynchronizeGuestOverlaps. Ele submete e tenta novamente
  quando uma escrita do executor ainda está pendente, antes de ler a textura.
  Não cria uma relação persistente para todos os overlaps parciais. Não foi portado.
- #395, checkpoint `ad1ed036`, já descrevia a lacuna A→B→A e um experimento de
  ponte RT. Esse resultado foi reaproveitado como controle, não anunciado como
  descoberta inédita. A nova evidência mede os bytes na master, nos dois modos
  Free Guest, e acrescenta o produtor omitido e consumidores de readbacks antigos.
  Não repeti o experimento de grafo nem os testes de descarte/padding já concluídos.

## Caminhos reais comparados

| Caminho | O que faz nesta base | Limite demonstrado |
| --- | --- | --- |
| CommandExecutor::SubmitInternal | Ordena ciclos por textura, espera a submissão anterior, sincroniza host e mantém dependências de lifetime | A e B possuem ciclos e backings diferentes; a associação física guest não vira uma transferência de dados entre esses backings |
| TextureManager::FindOrCreate | Reutiliza backing compatível, registra certos aliases exatos ou recupera `matches` antes de criar uma imagem | Retornos de cache ocorrem antes do readback de overlaps; um subrange contido que não é mip/layer representável pode não entrar em `matches` |
| RegisterAliases | Exige mapeamentos físicos ordenados iguais e layouts reversíveis; serial/geração escolhem autoridade dentro do grupo | A/B dos casos parciais não ingressam no mesmo grupo; não passam por essa escolha de escritor |
| RecordGpuWrite | Registra serial e invalida os pares de um AliasGroup existente | Não invalida a outra imagem parcial sem grupo |
| SynchronizeHost / Inline | Faz upload quando a própria textura é CpuDirty | Não consulta a escrita mais recente em outra imagem parcial; Clean pode significar apenas que esta imagem não percebeu mudança |
| SynchronizeGuest | Codifica a imagem GPU deste objeto para seu mirror guest | Pode codificar bytes que já ficaram antigos em parte do intervalo; conclusão da fence não os torna atuais para todas as interpretações |
| FreeGuest | Sob as condições existentes, pede liberação do mirror e marca memoryFreed | Não registra dependências parciais nem torna outra representação stale; descartar bytes não transfere autoridade |

O executor não é modelado integralmente pelo novo teste. Suas funções foram
inspecionadas no código. O teste torna o argumento mais restrito: cada produtor
Vulkan é submetido e tem sua fence explicitamente esperada **sem readback guest**
antes do próximo consumidor. Portanto os defeitos observados não dependem da
escrita ainda estar na fila, do atraso da GPU ou da corrida da #480.

## Reprodutor determinístico

Reuso do harness real-Vulkan `tests/gpu_backing_promotion`, com os métodos atuais
de FindOrCreate, aliases, upload, readback, encoders e tracking extraídos da
produção. Clear de attachment, imagens, staging, comandos, fences e comparação
de bytes GPU são reais. Allocator/scheduler de integração, mirrors e traps têm
as fronteiras host já documentadas pelo harness.

As imagens têm formato RGBA8 nativo, um mip/camada, Tegra block-linear. Não são
capturas HDR do jogo. Os intervalos abaixo são offsets relativos a uma alocação
host alinhada; não são endereços observados no Moto G100:

| Recurso | Dimensões | Intervalo físico guest, fim exclusivo |
| --- | --- | --- |
| A | 128×128 | [0, 65536) |
| B | 128×128 | [49152, 114688) |
| C | 128×256 | [0, 131072) |
| D, caso contido separado | 64×64 | [4096, 20480) |

A/B compartilham 16384 bytes. O clear parcial de A afeta apenas o primeiro GOB,
512 bytes fora de B. O teste verifica esses bytes antes do consumidor. A área de
clear é definida por renderArea; ver a semântica primária de
[VkRenderPassBeginInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderPassBeginInfo.html).

### Casos e controles

| Caso | Sequência relevante | Resultado real da master e da #480 |
| --- | --- | --- |
| 0, cache conhecido | A vermelho → criar B → B verde → reutilizar A | A conserva vermelho no intervalo compartilhado. Criar C posteriormente recupera B e produz a combinação correta: controle que distingue criação de reutilização |
| 1, controle de ordem | A vermelho → B verde → A inteiramente azul → criar C | Correto: a ordem B→A dos readbacks coincide com a ordem das últimas escritas |
| 2, autoridade parcial | A vermelho → B verde → primeiro GOB de A azul → criar C | B verde é codificado, depois A inteira recoloca vermelho antigo no intervalo de B. C recebe esses bytes incorretos |
| 3, ordem incompatível | A vermelho → B verde → A inteiramente azul → B verde novamente → criar C | A tem serial mais antigo que B, mas é codificada depois. C recebe azul antigo na região que B acabou de escrever verde |
| 4, produtor omitido | A vermelho concluído → criar D contido, sem mip correspondente | A não entra nos `matches`; seu serial de readback continua atrasado. D envia sentinela 0x2D com OFF ou zeros no modelo ON, em vez do vermelho atual |

No caso 4, recuperar A explicitamente, invalidar D no fixture e repetir o upload
produz vermelho correto. Esse é um **controle de causalidade do teste**, não uma
alteração introduzida no emulador.

Os testes preservam os resultados incorretos como observações negativas. Um
`PASS audit` significa que as falhas e os controles foram reproduzidos; não que a
coerência foi corrigida. O texto `*_bytes=16384` nos logs delimita o intervalo
afetado, não afirma que cada byte RGBA do intervalo seja diferente.

### Por que serial global não resolve

O índice visita os overlaps em endereços descendentes, B→A neste fixture, não
pela ordem de escrita. Ordenar por gpuWriteSerial corrigiria o caso 3, mas não o
caso 2: nele A realmente tem serial mais novo, só que a escrita nova não atingiu
a região compartilhada. A imagem A contém simultaneamente bytes novos e antigos.
Uma fence, um estado GpuDirty ou o maior serial para a imagem inteira não descreve
essa autoridade parcial. O caso 2 é um contraexemplo ao patch de ordenação.

## Free Guest ON/OFF: conclusão exata

O fixture existente implementa FreeMemory preenchendo o mirror com zeros; não
executa MADV_REMOVE nem reproduz falhas de tradução de endereço. Os layouts usados
aqui têm payloads completos/alinhados e evitam o problema separado de padding.
Ainda assim, o resultado ON é uma observação **desse modelo de descarte**, não
prova de que os mesmos bytes sejam descartados no Android.

Em ambos os modos, a criação omitida consome uma cópia sem a escrita GPU. OFF
retém o sentinela antigo; ON troca-o por zeros. Nos outros casos, ON não impede
que uma imagem antiga sobrescreva a representação guest ou que um backing de
cache permaneça antigo. Logo, a opção não garante o invariante investigado.

A #480 já registrou que `free-applied` nos logs significa chamada e flag, não
sucesso comprovado do descarte físico. Não inferi preenchimento por zeros no jogo
a partir desses contadores, nem associei o sentinela do teste a NaNs/HDR reais.

## Validação executada

- 5 cenários × 2 modos Free Guest × 2 modos de format-list = **20 execuções** na
  master; outras 20 usando os métodos da #480. Mesmos resultados observáveis.
- ASan/UBSan em ambas as execuções; Vulkan synchronization validation com **zero
  erros**. Cada produtor terminou antes do consumo; não há espera baseada em sleep.
- Controles de bytes GPU para A, B e C e recuperação explícita do caso contido.
- Nenhuma suíte já concluída foi repetida em sua totalidade. O teste preexistente
  CheckFreedWriterGuestBridge continua fora desta auditoria focada; seu resultado
  negativo permanece registrado na #480. Não há alegação de suíte completa verde.
- Leak detection desativado no host. Nenhuma validação gráfica ou de desempenho
  Android foi feita. Os hashes das funções comparadas estão no checkpoint.

Resultados: `docs/checkpoints/physical-authority-20261122/`.
Comandos e fronteiras: `tests/gpu_backing_promotion/PHYSICAL_AUTHORITY.md`.

## Bloqueio para uma correção segura

Há uma falha genérica reproduzida, mas não há uma correção completa validada nem
ligação causal com SpongeBob. Acrescentar um produtor à lista de readbacks resolve
a omissão local apenas em histórias simples; não fornece invalidação persistente
e pode publicar dados antigos quando existem vários escritores parciais. Ordenar
cópias inteiras também foi refutado. Não publiquei nenhum desses patches.

Uma correção precisa preservar a relação física ao criar **e ao reutilizar** os
recursos, recuperar a fonte ainda válida antes de perder sua autoridade e
atualizar o consumidor antes de uma escrita parcial. Deve abranger descritores
retidos, RT/storage, falhas de recuperação e origem de escritas ainda não
submetidas; não basta adicionar uma barreira Vulkan.

O experimento anterior da #395 já identificou riscos de retry durante preparação
de descritores. Não o importei. Nem todos os detalhes daquele baseline podem ser
transportados: na master atual, a conversão de quads ocorre depois de PrepareDraw.
Por outro lado, GetStorageTexture ainda resolve/associa imagens e chama
MarkGpuDirty dentro da preparação de descritores. SnapshotBindlessHandles captura
handles; não estabelece por si uma transferência de autoridade entre imagens
parciais. Projetar essa transferência exige um limite transacional que ainda não
foi demonstrado para todos esses consumidores. Um grafo parcial restrito a RT não
seria uma solução genérica segura para publicar agora.

## Próximo experimento mínimo

Para testar a relação com o jogo, a próxima coleta deve observar **uma única
família de intervalos físicos** no menu, não repetir #478/#479 nem capturar um
frame inteiro. Usar instrumentação temporária limitada, por exemplo até 8 imagens
e 64 eventos da família, nos caminhos já executados:

1. Registrar escrita efetiva/clear e intervalo, identidade/geração do backing,
   serial, layout, formato, faixa e origem da relação de aliases.
2. No lookup/reuso/upload, registrar quem foi considerado, quem entrou em matches,
   quem foi ignorado e os estados/seriais dos produtores sobrepostos.
3. Nos readbacks/uploads **que já ocorreriam**, registrar ordem e hashes dos
   bytes compartilhados antes/depois; registrar descarte real e resultado da API
   se essa família for liberada. Não criar cópias ou esperas para a coleta inicial.
4. Confirmar que o produtor foi submetido e identificar a extensão efetivamente
   escrita. Serial maior sozinho não prova autoridade para todo o intervalo.

O critério de confirmação é ligar um consumo visivelmente problemático a uma
faixa comprovadamente mais nova que foi ignorada/sobrescrita, ou a uma fonte
recuperável omitida; apenas um overlap ou diferença de hash entre boots não basta.
Se os pontos equivalentes OFF/ON não passarem por esses caminhos, esta classe de
falha não explica aquela melhora e a hipótese deve ser descartada para o caso.
Essa coleta ainda não foi implementada: evita outra APK sem primeiro aproveitar
a validação da #480 e escolher uma família efetivamente usada pelo menu.

## Fontes preservadas

- [#480 e relatório anterior](https://github.com/BBlueSkyy/My-fork-Strato/blob/8453cc6f10500a7cf6a99bf14f85eaf66e50d489/docs/gpu/cosmic-freeguest-authority-20261122.md)
- [#460, helper de readback pendente](https://github.com/BBlueSkyy/My-fork-Strato/blob/579955a318cd00a632e3000a21c4ca2252f05730/app/src/main/cpp/skyline/gpu/texture_manager.cpp)
- [#395, checkpoint de recuperação e experimento de grafo](https://github.com/BBlueSkyy/My-fork-Strato/blob/ad1ed036/docs/guest-memory-recovery.md)

A comparação anterior com Kenji-NX/Ryubing/Eden permanece válida apenas dentro
dos limites documentados na #480: dependências e regiões fazem parte das suas
arquiteturas, não são campos isolados copiáveis. Não reabri essa comparação.
