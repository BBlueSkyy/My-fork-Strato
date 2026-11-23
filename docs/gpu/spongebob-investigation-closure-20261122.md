# #481 — encerramento da investigação do menu do SpongeBob

Base preservada: `03dcc9c4a384e315195bd8afeb619ca0332310b7`, continuação de
40e42308 e ec8e8b89. Esta rodada modifica apenas documentação e evidências.
Não há correção de renderer, nova instrumentação, build, merge ou pedido de teste.

## Decisão

Não há evidência suficiente para uma correção localizada segura da corrupção
visual. A investigação é encerrada nesta rodada, conforme solicitado. A hipótese
de autoridade incorreta durante a expansão permanece plausível; não está provada
nem descartada. Não se declara o jogo corrigido, nem a expansão inocente.

Os três defeitos físicos de ec8e8b89 permanecem confirmados no reprodutor, mas não
foram reexecutados nem elevados a causa visual do jogo. A corrida #480 não foi
reinvestigada. #476 não resolveu o visual; #478/#479 já separaram política genérica
gpuDirty da opção Free Guest. Não foram repetidas variantes A/B.

## Comparação dos arquivos originais

O ZIP contém somente dois logs, sem capturas. A classificação visual abaixo foi
fornecida pelo usuário. Ambos são Free Guest OFF, sampling=mip-growth-v3.
Hashes SHA-256 estão em sources.json; os eventos GPU-PHYS foram preservados sem o
restante dos logs. compare.py reproduz as verificações sem executar o emulador.

| Observação | Boot 1: verde/vermelho | Boot 2: preto/brilho |
| --- | --- | --- |
| Registros incluindo config | 62 | 63 |
| Último seq / lost / image_limit | 61 / 4 / 1 | 62 / 0 / 1 |
| Prefixo de upload base: NaN / finitos >1000 / canais | 34 / 26 / 1024 | 0 / 0 / 1024 |
| Primeiro growth-route | 8 solicitado, permitido, freed=0 | Igual |
| Crescimento | 1→8, geração 1→2 | Igual |
| Escritores-filhos | 7 GPU-dirty, submissão observada, sem amostra GPU | Igual |
| Readback serial dos sete filhos | 0 | 0 |
| Amostras de expansão guest=staging | 12/12 | 12/12 |
| Pares de texels que diferem entre boots | 12/12 | 12/12 |
| Primeiro consumidor de nível adicionado | mip 1, um nível, seis faces | Igual |
| Destino do primeiro consumidor | imagem 57, RGBA16F 32×32, mip 0/camada 0 da view | Igual |

A ordem dos primeiros 52 registros (config e seq1–51) é idêntica. Todos os seriais
não zero comparáveis no mesmo ponto diferem por uma translação constante de 525;
isso não demonstra reordenação. Endereços físicos, handles e ponteiros de ciclos
são específicos de cada sessão. As bases devem ser normalizadas, não comparadas
como se apontassem para o mesmo recurso nos dois processos. A partição dos ciclos
observados dos filhos também é a mesma: 64/32 juntos, 16/8/4/2 juntos, 1 separado.
Não há prova de diferença de sincronização que explique as duas cores finais.

A diferença mais precoce registrada de conteúdo já existe no upload inicial:
o hash guest da janela física é 15052919760952323741 no boot1 e
9261658015842282352 no boot2. Esses hashes não são hashes do prefixo HDR de staging.
As 12 amostras da expansão, ao contrário, comparam precisamente o mesmo texel:

| Mip/camada | Offset guest | Offset staging | Bits boot1 | Bits boot2 |
| --- | ---: | ---: | --- | --- |
| 1/0 | 131072 | 0 | 0000000000000000 | 00ff541900ff5419 |
| 1/1 | 311296 | 32768 | 0000000000000000 | 0000bacf0000bacf |
| 1/2 | 491520 | 65536 | 0000000000000000 | 00005da800005da8 |
| 1/3 | 671744 | 98304 | ffc0dfe7ffc0dfe7 | 00005da800005da8 |
| 1/4 | 851968 | 131072 | 0000000000000000 | 00005da800005da8 |
| 1/5 | 1032192 | 163840 | cf25bdad00000000 | 00ff531800ff5319 |
| 2/0 | 163840 | 196608 | 0000000000000000 | 0000bbcf0000bbcf |
| 3/0 | 172032 | 245760 | 0000000000000000 | 0000bbcf0000bbcf |
| 4/0 | 174080 | 258048 | 0000000000000000 | 00ff531a00ff551c |
| 5/0 | 174592 | 261120 | 0000000000000000 | 0007bac900a1ad0b |
| 6/0 | 175104 | 261888 | 0000000000000000 | 0000bbcf0000bbcf |
| 7/0 | 175616 | 262080 | 0000000000000000 | 0000bbcf0000bbcf |

Cada endereço físico satisfaz base do pai + offset guest; a camada usa stride
180224. As amostras são (0,0), não cobrem os mips inteiros. O texel 1/3 boot1
contém dois canais NaN. Nenhum dos doze texels boot2 contém NaN. Valores negativos
ou padrões repetidos não identificam sozinhos conteúdo não inicializado, origem
CPU ou readback obsoleto. Nenhum log identifica quem escreveu esses bytes.

## Caminho real auditado

1. TextureManager::FindOrCreate considera fullMatch se a geometria/tile/stride
   coincide e a view cabe após CanGrowMipLevels. Esse retorno antecede o fallback
   que recupera matches. ReconcileAliases/GetView só recuperam grupos registrados;
   os estados conhecidos desta família registram group=0/group_known=0.
2. CanGrowMipLevels valida tipo, formato nativo reversível, layout dos níveis
   antigos e capacidade dentro do stride/mirror; memoryFreed rejeita crescimento.
   A validação de capacidade não valida a autoridade atual dos bytes.
3. A exceção TextureBackingPromotionRequest faz o executor Submit e repetir a
   preparação. HandleTextureBackingPromotion não ignora o trabalho anterior e
   não reutiliza os descritores interrompidos. O lease mantém guest acessível,
   mas acessibilidade não significa atualidade.
4. PromoteBackingForView chama SynchronizeHost(false) somente no pai, conserva
   previousCycle, aguarda submissão e encadeia esse ciclo ao copyCycle. O mip0 é
   copiado da GPU. Os mips1–7 vêm do mirror por deswizzle para initialization.
   Não consulta os filhos para substituir a origem desses níveis.
5. RecordBackingPromotion transiciona os níveis novos, copia o mip0 e envia
   initialization com bufferOffset por mip e seis camadas. As fórmulas amostradas
   coincidem com os layouts/strides esperados; não se observou troca de offset ou
   corrupção da cópia guest→staging nos texels examinados. As barreiras/lifetime
   protegem os recursos Vulkan envolvidos, não transferem autoridade dos filhos.
6. A promoção troca backing, layouts, quantidade de níveis, geração e invalida
   downloadStagingBuffer. Views antigas e framebuffers são retidos para trabalho
   anterior. Os consumidores posteriores têm backing_generation=2.
7. SubmitInternal mantém ciclos por objeto Texture e faz upload apenas se o
   objeto estiver CpuDirty. RecordGpuWrite invalida membros de um AliasGroup,
   não todas as representações físicas parciais. SynchronizeGuest espera sua
   própria GPU e codifica seu layout inteiro. Isso preserva a lacuna já reproduzida.

Não foi encontrada neste caminho evidência nova de barreira Vulkan incorreta,
use-after-free de staging/view ou descarte OFF. Isso não equivale a validar todos
os acessos GPU do jogo por logs CPU. As amostras coincidentes refutam somente um
erro de cópia nos pontos amostrados, não uma origem guest obsoleta.

## Consumidor: o que ficou comprovado

Os dois boots usam os hashes Maxwell FFF53A080206C6D8 e 63C97A560DB89158 no primeiro
draw preparado: view Cube, mip1, levelCount1, seis faces, destino filho32×32.
O registro seguinte base-only usa o mesmo par e destino64×64. É uma aresta dentro
da cadeia de cubemap; não é uma aresta demonstrada ao render target HDR final do
menu. Não há código de shader desses hashes nos anexos, nem valores de saída.

O sampler registrado tem clamp [0,15], bias0, mas detail[6]=1 nos dois casos:
a associação ao binding é ambígua. A view de um nível limita o acesso ao mip1
físico, independentemente de permitir clamp15 relativo à view. Não se afirma que
o draw lê oito níveis ou que o shader chega à iluminação final. Registro de
preparação não certifica execução, cobertura, valores finais ou fragmentos ativos.

Os growth-submitted posteriores não representam treze promoções. O recorder usa
um flag growthCycle por imagem e atualiza o ciclo associado; notificações seguintes
recebem o mesmo rótulo. Só o context do growth-recorded identifica a submissão da
promoção. Nenhum desses registros certifica conclusão da fence GPU. A cauda não
tem end/budget_end: ausência de readback-published é ausência na janela observada,
não prova de que nunca houve readbacks no processo.

## Comparação com código real Kenji-NX

Checkout disponível: a6a641ada215837297e7170c91c6b4e8275574c6,
origin https://git.ryujinx.app/projects/Kenji-NX.git. É referência de código local;
não se afirma que seja a mesma versão binária testada pelo usuário.

- Image/TextureGroup.cs::CreateCopyDependency(Texture,int,int,bool) subdivide
  handles e relaciona intervalos de níveis/camadas de duas representações.
- TextureGroup.cs::SynchronizeMemory avalia handles relevantes, privilegia GPU
  Modified, executa NeedsCopy/Copy e permite SynchronizePartial. CPU dirty não
  determina sozinho um reupload completo que apague partes ainda atuais na GPU.
- TextureGroupHandle.cs::Copy verifica DeferredCopy/Modified/dirty e usa CopyTo
  com níveis/camadas de origem/destino para layouts compatíveis.

A diferença concreta é dependência persistente por subrecurso, além de ordenar
submissões Vulkan. O Strato auditado não estabelece essa relação geral entre pai
promovido e filhos. Isso explica por que esperar fences isoladas não basta, mas
não prova que copiar esta arquitetura resolva o jogo. Uma cópia incondicional de
filhos ou readback de todos os overlaps não replica as regras de validade e de
invalidação CPU do Kenji e pode reintroduzir os casos parciais de ec8e8b89.

## Hipóteses e bloqueios finais

- Sequência de expansão diferente entre os dois boots: não sustentada na janela
  comparável; ordem/consumidor coincidem, seriais têm apenas offset constante.
- NaNs como causa necessária de toda corrupção: não sustentada; boot2 é corrompido
  sem NaNs nas amostras. Não se excluem NaNs em áreas não amostradas.
- Deswizzle criando NaNs nos doze texels da expansão: refutado nesses pontos pela
  igualdade bit a bit. Os bits já são guest. Não se identifica seu produtor.
- Readback antigo sobrescrevendo guest nestes boots: não demonstrado. O defeito
  reduzido permanece válido, mas nenhum readback publicado foi capturado aqui.
- Filho GPU mais recente omitido: estruturalmente possível e apoiado por estados
  GPU-dirty/submissão sem amostra; faltam bytes/cobertura/autoridade efetivos por
  região para provar que o texel correto difere daquele enviado ao pai.
- Free Guest ON: os dois anexos finais são OFF. Os logs ON anteriores explicam
  zeros após descarte físico abrangente e a rejeição estática por memoryFreed,
  mas não mostram o mesmo crescimento/consumo final para uma comparação causal.
- Relação com iluminação: demonstrada somente a passagem pai→draw→filho32×32;
  ligação aos pixels finais do menu continua ausente.

Esses bloqueios impedem atribuir causa-raiz visual e escolher uma correção mínima
segura. Zerar mips, suprimir crescimento, copiar filhos inteiros, ordenar todos
os readbacks por serial ou forçar Free Guest seriam intervenções não validadas.
O caso parcial anterior impede substituir essa análise por uma regra de autoridade
da imagem inteira. Nenhuma dessas intervenções foi implementada.

## Verificação e encerramento

compare.py verifica 24 correspondências guest/staging, offsets físicos, sete
escritores por boot, sequência comparável, delta525, consumidor e doze diferenças
entre boots. O resultado está em comparison.json. Não é teste de correção GPU.
Os testes Vulkan/ASan/UBSan anteriores continuam preservados; não foram repetidos
para um patch exclusivamente documental e não comprovam os dados do jogo.

Não há correção funcional ou APK nova nesta rodada. A #481 permanece Draft,
sem merge. Não será solicitado outro ciclo de logs ou testes no aparelho.
