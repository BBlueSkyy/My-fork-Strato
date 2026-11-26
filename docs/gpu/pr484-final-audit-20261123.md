# #484 — auditoria final, ainda Draft

HEAD de produção auditado: c57978a67697650cfa7f934a69ed538cf8aa2157.
Branch fix/isolate-mk8-smo-overlap-sequencing-20261122; base 8bcaee0b.
Os quatro commits 3c243818, 1622c03b, f5ba3022, c57978a6 foram preservados.
Nenhuma alteração final em arquivos de produção. Nenhum squash, merge ou nova PR.
As #459/#460/#483 permanecem intactas. Nenhuma instrumentação ou APK nova.

## Resultado e critério de merge

O mecanismo de pending writes é fundamentado: uma Texture pode ter trabalho novo
nos nodes do executor enquanto cycle ainda aponta para trabalho anterior. O receipt
pendingGpuWriteTag identifica esse caso para o contexto que já possui a textura;
a reconciliação força Submit antes do readback e retoma a seleção. Não é um sistema
de autoridade por subrecurso nem uma garantia para todos os overlaps parciais.

Não encontrei uma nova violação reproduzida de sequenciamento no mecanismo isolado.
Os controles adicionados passam. Entretanto, a suíte completa exigida não passa:
CheckFreedWriterGuestBridge lança antes de restaurar a memória guest descartada.
O caso também falha com os métodos da master; RegisterAliases é idêntico nela.
A recomendação final é **manter Draft, sem recomendar merge incondicional** até
resolver ou delimitar formalmente esse bloqueio de Free Guest ON. CI verde anterior
não substitui o resultado vermelho da suíte Vulkan completa.

O A/B do usuário é preservado como evidência gráfica: A não resolveu os dois casos;
B resolveu neblina SMO e seleção MK8 no Moto G100. O mecanismo validado não mudou.
Não repeti o A/B nem alego novo teste de BOTW/RE5. O brilho rosa/branco MK8 permanece
fora de escopo. Também não retomei a investigação do SpongeBob.

## Auditoria de sincronização e lifetime

- RecordGpuWrite publica o tag sob a posse do recurso. O serial histórico não é
  tratado como pending write. O tag corresponde ao contexto executor; não certifica
  conclusão GPU nem cobertura espacial de um draw.
- SynchronizeGuestOverlaps pré-verifica todos os produtores antes de ler qualquer
  byte guest. Somente tag não nulo, posse pelo mesmo contexto e receipt coincidente
  exigem a exceção. Isso evita esperar o ciclo antigo e publicar dados anteriores.
- TextureAliasReconciliationRequest mantém shared_ptr dos produtores. O unwind sai
  do lookup; o handler executa Submit, depois readbacks, depois registro/reconciliação.
  O mesmo pedido pode conter duplicatas; locks são deduplicados. Readback repetido
  do mesmo recurso já Clean retorna sem nova cópia.
- SubmitInternal ordena o ciclo anterior, sincroniza host e publica texture->cycle;
  só depois limpa pendingGpuWriteTag. Assim o receipt é substituído pela dependência
  de ciclo. ResetInternal libera os anexos; LockedTexture::~LockedTexture chama unlock.
- Texture::unlock limpa receipt e tag antes de liberar o mutex. Isso cobre o fim de
  uso e o cancelamento/destruição dos recursos anexos; um serial maior pode restar,
  mas não causa loop de retry por si só. Tag relaxed é um receipt usado sob posse
  do contexto/recurso, não publicação independente de bytes ou ciclo para leitores
  sem lock. Esse contrato depende de não compartilhar o mesmo contexto entre
  produtores concorrentes, como já exige o executor.
- A retomada limpa o estado de preparação pelo Submit/retry existente. Não se copia
  um descriptor/allocator intermediário de volta para a operação interrompida.
  Falha de Submit propaga antes do readback; não se afirma rollback transacional de
  todo SubmitInternal em exceções fatais de alocação/driver.
- As referências do pedido, attachments e ciclos mantêm recursos vivos nas esperas.
  Não foi adicionado dono global, grafo ou retenção extra entre frames.

## Locks e trapping

SynchronizeGuestOverlaps ordena os Texture locks por endereço antes de tomar os
stateMutex. Não segura mutex de metadados de AliasGroup enquanto aguarda GPU.
A ordem original de readbacks é mantida: ordenar imagens por serial não resolveria
a autoridade parcial e não foi feito.

O stateMutex permanece adquirido no bridge SynchronizeGuest(false,true). Isso
impede callbacks CPU de observar Clean/memoryFreed intermediários antes de terminar
a cópia. Falha restaura dirtyState/memoryFreed ainda sob esse lock. SynchronizeGuest
rejeita aliasStale e CpuDirty, evitando readback que sobrescreva bytes CPU atuais.

Há aquisição interna de operationMutex por SynchronizeGuest. A aparência de ordem
state→operation neste helper merece atenção, mas não é por si um deadlock provado:
os recursos participantes já estão travados; os callbacks ReadTrap/WriteTrap usam
try-lock de estado/recurso/operação e retornam false, e ReconcileAliases solta a
operação antes de adquirir o writer. Não alterei essa ordem sem um ciclo de espera
alcançável reproduzido. O teste novo exercita duas threads com listas em ordem
inversa; não é prova exaustiva de todos os interleavings Android.

O helper não adquire o TextureManager nem carrega iteradores para o handler. O
pedido leva recursos, não locks do manager. O contrato externo de serialização dos
lookups permanece o mesmo. Caminhos de trap não invocam FindOrCreate por este patch.

## Lacuna concreta dos testes e melhorias realizadas

O harness existente compilava o helper, mas ContextLock ignorava tags e os testes
não produziam um receipt pending explicitamente. Agora:

- ContextLock é extraído de tag_allocator.h; LockWithTag e unlock vêm de texture.cpp.
  TextureView faz apenas a adaptação do shared_ptr do fixture para LockWithTag.
- O novo pending_overlap_cases.h executa RecordGpuWrite, SynchronizeGuestOverlaps,
  HandleTextureBackingPromotion(request), SynchronizeGuest, upload/readback reais
  e o cleanup de tag. Verifica exceção antes de tocar guest, shared ownership,
  submissão antes de readback via callback de fronteira, retomada finita,
  preservação de CpuDirty mais novo, rollback de falha de alocação, retry,
  duplicatas sem nova cópia e posse de lock pelo mesmo contexto.
- Duas threads fazem 100 chamadas cada, com fontes em ordens inversas; os locks
  ordenados permitem conclusão. Repetido para OFF/ON e format-list OFF/ON.
- O allocator/scheduler/fence do harness continuam fachadas de interface apoiadas
  por objetos, comandos e fences Vulkan reais. O Submit do executor é uma fronteira
  controlada de teste; não se afirma executar toda a thread de gravação Android.
- A facade foi atualizada para sparseSegments, necessário para compilar os corpos
  atuais. ReadSparseBacking lança se for usado: casos aqui são residentes nativos,
  e nenhum teste sparse foi substituído por um resultado falso de sucesso.

O default da suíte continua executando todos os testes e continua vermelho no
bloqueio identificado. Os modos focados PENDING_ONLY, PROMOTION_ONLY e
FREED_BRIDGE_ONLY permitem localizar resultados e o controle negativo sem esconder
ou desabilitar a falha da suíte completa. TEXTURE_REF seleciona corpos de produção
de um commit para distinguir falha preexistente de regressão.

## Resultados dos testes

| Verificação | Resultado |
| --- | --- |
| gpu_sampled_layout/run.py, ASan/UBSan | Passou: 20 oracles e aliases/RT/cache/retry |
| gpu_tic_validation/run.py, compilador na revisão 9cdd405 | 1186 checks passaram |
| gpu_tic_validation/run_vulkan.py, UBSan | 7 dimensões sampled + storage + bindings reais; zero erros Vulkan |
| Backing PENDING_ONLY, ASan/UBSan | Passou OFF/ON e format-list OFF/ON; pixels GPU→guest conferidos |
| Backing PROMOTION_ONLY, ASan/UBSan | Passou: promoção, mip growth 1→8/seis faces, views cached e comprimidas |
| Backing default, ASan/UBSan | FALHOU: CheckFreedWriterGuestBridge, Cannot losslessly reconcile these texture alias layouts |
| Mesmo caso com TEXTURE_REF=8bcaee0b | Mesma falha; não introduzida por 1622c03b |
| Tentativa GCC do backing | Não compila: ordem de designated initializers dos corpos de cópia Vulkan preexistentes; execução Vulkan feita com Clang |
| Produção/kernel/scheduler/apresentação desde c57978a6 | Nenhuma alteração |

Compilador das execuções Vulkan: Clang 19, compatível com o uso de inicializadores da base Android. A falha de compilação GCC não foi ocultada nem corrigida por refatoração fora de escopo.

ASAN_OPTIONS=detect_leaks=0, como os harnesses anteriores. Não é auditoria de leaks.
A validação Vulkan do backing usa synchronization validation. Logs e metadados estão
em docs/checkpoints/pr484-final-audit-20261123/.

## Bloqueio Free Guest ON e patch rejeitado

RegisterAliases sem layout explícito chama CanReconcileGuestLayout da referência
antes de alcançar restoreNewest. CanReconcileGuestLayout rejeita memoryFreed.
CheckFreedWriterGuestBridge cria um escritor nativo recuperável, descarta guest e
pede registro com outro layout; encontra essa rejeição antecipada.

Foi avaliado localmente mover a mesma validação estrita para depois da restauração.
A rejeição desapareceu, porém o teste falhou depois em bytes==expected: readback
recupera texels, não todos os bytes do span de strides descartado. Padding/tails
não representados na imagem não podem ser reconstruídos por mera espera/cópia.
Aquela mudança foi integralmente descartada. Não se enfraqueceu a validação nem
se redefiniu expected para fazer o teste ficar verde. Essa observação é compatível
com as limitações já documentadas em #481/ec8; não justifica ampliar esta PR para
um sistema regional de autoridade ou importar #400.

O bug de rejeição e a garantia de preservação integral precisam ser tratados juntos
ou ter um domínio de recuperação explicitamente certificado. Não posso recomendar
uma alteração em produção que apenas transforme a exceção em publicação de bytes
incompletos. Isso também impede afirmar que todo caminho Free Guest ON esteja seguro.

## Estado final e integração

O CI de c57978a6 passou na execução 45100015110. Esse workflow não executou a suíte
completa de backing que revelou o bloqueio acima. Esta atualização contém testes e
relatório, preserva a produção byte a byte e usa skip ci para não gerar outra APK.
Não se anuncia CI novo verde. Os quatro commits existentes foram mantidos.

As correções gráficas da versão B permanecem no mesmo código testado no Moto G100;
não há necessidade de repetir A/B por esta atualização exclusivamente de testes.
Se o bloqueio for corrigido futuramente com mudança funcional de readback/Free
Guest/seqüenciamento, será necessária uma Release ARM64 correspondente antes de
merge, pois a validação gráfica de B não cobre aquele novo comportamento.

Recomendação: PR #484 permanece Draft e bloqueada para a recomendação final de
merge nas condições solicitadas. Não há regressão nova demonstrada pela auditoria,
mas ainda existe um teste relevante de preservação de dados falhando; isso não deve
ser ocultado pelo CI anterior nem pelo sucesso visual de dois jogos.
