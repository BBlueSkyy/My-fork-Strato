# PR #481 — conteúdo dos uploads OFF/ON no Moto G100

Base de código preservada: `295abbf41b10fed93b28fc9a1d76269aa790e476`.
Nenhuma mudança de renderer, sequenciamento, política Free Guest ou build Android.
Os três reprodutores de ec8e8b89 e a auditoria da #480 não foram repetidos.

## Fontes e resultado

Anexos originais lidos integralmente, com classificação visual fornecida pelo usuário:

| Arquivo | Resultado visual | SHA-256 |
| --- | --- | --- |
| log 1 free guest off.log | Gravemente corrompido | 8b92829e36eb0dd1cc4331d2623ec83572c7d8df70ff5fa0cd1d2037e66a05cb |
| log 2 free guest on.log | Quase correto | 63b3d2c9688555fef4da104d5758326c36a2e5a38faf600778ceed084c8066b7 |

Os hashes coincidem com as cópias dos logs já obtidas nesta continuação. Não há
um terceiro experimento nem novas capturas GPU. Foram disponibilizados diretamente
dois logs nesta mensagem; a classificação das imagens é a do usuário.

**Confirmado:** existem NaNs no prefixo de staging preparado em OFF; os uploads
menores ON têm hashes compatíveis com zeros após descarte bem-sucedido de outra
representação fisicamente sobreposta. Isso explica a aparente ordem invertida
"zeros antes do descarte da própria textura" sem inventar um descarte futuro.

**Ainda não confirmado:** quais escritas originaram os bytes iniciais, se algum
conteúdo GPU efetivamente diferente foi perdido e se essa família é consumida
pela iluminação do menu. Não se atribui a corrupção ao sequenciamento apenas
pela existência de overlaps ou seriais diferentes.

## O que foi realmente amostrado

`upload-source.hash` e `upload-prepared.hash` são ambos hashes de até 4096 bytes
do **mirror guest**, na interseção física escolhida. O segundo não é um hash do
VkBuffer de staging. Já nan/excess/samples em upload-prepared vêm do **prefixo
linear do staging** existente, até 256 texels, quatro canais por texel neste caso.
O log não contém os valores half brutos, contagem de bytes zero ou hash desse prefixo.

| Tamanho | OFF: canais NaN | OFF: canais finitos >1000 | Canais examinados | ON: NaN / >1000 |
| --- | ---: | ---: | ---: | ---: |
| 128×128 | 139 | 3 | 1024 | 0 / 0 |
| 64×64 | 0 | 0 | 1024 | 0 / 0 |
| 32×32 | 0 | 0 | 1024 | 0 / 0 |
| 16×16 | 4 | 18 | 1024 | 0 / 0 |
| 8×8 | 126 | 5 | 1024 | 0 / 0 |
| 4×4 | 32 | 0 | 384 | 0 / 0 |
| 2×2 | 8 | 0 | 96 | 0 / 0 |
| 1×1 | 2 | 0 | 24 | 0 / 0 |

Essas contagens são de canais amostrados, não pixels NaN da imagem inteira. Nos
mips pequenos o prefixo pode atravessar faces. Não se somam as linhas como uma
medida de corrupção de um render target do menu.

O caminho nativo 97→97 de SynchronizeHostImpl faz deswizzle/cópia de bits; não
executa uma operação aritmética que produza NaNs. O prefixo do staging de upload
vem dos payloads guest copiados. Portanto os bits inválidos já estão no caminho
de origem guest antes dos draws dessa submissão. O log ainda não identifica
qual escrita anterior — CPU, GPU/readback ou conteúdo não inicializado — os gerou.
Não há evidência para atribuí-los ao compilador shader desta operação.

No 128×128, o prefixo amostrado é do mip base/primeira face. Seu hash guest,
porém, começa no offset lógico **131072**, início do mip seguinte. Logo o hash
do pai e do 64×64 pode coincidir sem que os prefixos HDR coincidam. Em OFF isso
ocorre: hash 2430777436954680766 nos dois recursos, mas 139 NaNs no prefixo do pai
e zero no prefixo do filho. Não é contradição; são posições diferentes.

CopyFromStagingBuffer registra copyBufferToImage usando o buffer preparado e as
regiões produzidas por GetBufferImageCopies. Essa é a ligação estática ao upload.
Faltam no log o handle do buffer, offsets das regiões e um valor GPU lido de volta
no mesmo ponto; upload-prepared sozinho não prova os bytes efetivamente vistos
por um shader posterior. As notificações de submissão são amostradas.

## Intervalos físicos, faces e mips

Cada representação inicial tem formato RGBA16F, seis camadas, um mip descrito e
stride de camada **180224 bytes**. Seus spans têm 1081344 bytes (6×stride).
As bases relativas à textura 128×128 são:

| Tamanho | Offset de base | Envelope conservador do mip em cada face, fim exclusivo |
| --- | ---: | --- |
| 128 | 0 | [0, 131072) |
| 64 | 131072 | [131072, 163840) |
| 32 | 163840 | [163840, 172032) |
| 16 | 172032 | [172032, 174080) |
| 8 | 174080 | [174080, 174592) |
| 4 | 174592 | [174592, 175104) |
| 2 | 175104 | [175104, 175616) |
| 1 | 175616 | [175616, 176128) |

Adicionar camada×180224 reproduz a disposição das seis faces. Esses envelopes,
incluindo alinhamentos conservadores, são disjuntos antes da expansão do pai.
Já os mapeamentos amplos de seis strides se sobrepõem e até se estendem além do
fim do span do pai. **Overlap dos spans não prova overlap dos texels escritos.**
Uma escrita no único mip base da 128×128 não é automaticamente uma escrita mais
nova no payload do 64×64. Isso impede declarar a violação só comparando seriais.

## Por que ON tem zeros antes do descarte local

No boot ON, a textura 53 (128×128) faz upload nos eventos 22/23 e descarte no 25.
O descarte retorna sucesso, cobre 1081344 bytes e começa exatamente no mirror
solicitado. SetupGuestMappings/CreateMirror estabelecem um alias do span guest;
não é uma cópia independente. As amostras posteriores dos filhos caem nesse span:

| Recurso ON | Evento upload-source | Descarte anterior que cobre a amostra |
| --- | ---: | --- |
| 54, 64×64 | 27 | 25, recurso 53 |
| 55, 32×32 | 32 | 25, recurso 53 |
| 56, 16×16 | 37 | 25, recurso 53 |
| 57, 8×8 | 42 | 25, recurso 53 |
| 58, 4×4 | 47 | 25, recurso 53 |
| 59, 2×2 | 52 | 25, recurso 53 |
| 60, 1×1 | 57 | 25, recurso 53 |

As sete amostras têm FNV-1a 13346994205906133797, igual ao hash de 4096 bytes zero.
É evidência forte de zeros nessas amostras, não um dump literal ou prova de que o
recurso inteiro esteja zerado. Descartes seguintes também cobrem várias delas.
O cruzamento usa o offset entre início real do syscall e mirror, validando que
ele e o tamanho cabem no mapeamento de um único span. Não compara endereços de
mirrors diferentes como se fossem o mesmo endereço virtual.

O mecanismo observado é **descarte de páginas compartilhadas por interpretações
de mips diferentes**. O pai descarta não apenas os texels do seu mip base, mas
também as regiões de outros mips dentro de seu span/stride. Isso pode mascarar
dados antigos ao substituí-los por zeros; não torna a memória coerente.

Por outro lado, o primeiro prefixo ON já não tem NaNs **antes** do evento 25, e
seu hash guest não zero difere de OFF. Portanto os descartes registrados aqui
não explicam toda a diferença inicial. Existem escritas/descartes anteriores à
seleção que não foram capturados; não se inventa sua origem.

## Escritores GPU e expansão observada em OFF

Nos primeiros draws registrados, o 64×64 está CpuDirty, com write-intent e serial
avançando, mas readback serial zero. A intenção é anterior à submissão e ao upload;
não significa que aqueles dados GPU já existiam no instante do lookup. Essa
distinção reutiliza o resultado da #460, sem portá-la nem reabrir sua auditoria.

Depois dos uploads iniciais, os registros OFF 38/39 notificam a mesma submissão
para pai e filho. No evento 40 o pai está com backing geração 2; nos eventos
41/42 seu layout guest já descreve **oito mips**, em vez de um. O código de
PromoteBackingForView explica essa transição: preserva os níveis existentes por
cópia GPU e inicializa níveis novos a partir do mirror guest, em outro staging
(`initialization`), via RecordBackingPromotion. Essa preparação não passa pelos
hooks de SynchronizeHostImpl que produziram a tabela acima.

Há, portanto, um candidato concreto **mais específico**: a expansão do pai pode
consumir guest antigo dos mips que já foram destinos de draws em imagens-filhas.
O fullMatch retorna antes do fallback de matches, como no limite já reproduzido
em ec8e8b89. Após crescer a oito mips, o payload do pai passa a abranger os filhos;
o argumento de disjunção inicial deixa de se aplicar.

Não se assume uma expansão equivalente em ON: CanGrowMipLevels rejeita memória
marcada como liberada, e a coleta ON termina antes de observar esse caminho.
A opção pode alterar também a escolha de representação, não só os bytes zerados.
O comportamento efetivo dessa escolha ainda não foi capturado.

Mas não foram registrados os dados GPU dos filhos, a cobertura efetiva desses
draws, os buffers/regiões da inicialização dos novos mips nem a binding/shader que
consome o pai expandido. Não se provou que os texels GPU eram diferentes dos
guest, nem que essa diferença chega ao menu. Os primeiros draws só têm bound-only,
sem clear explícito ou garantia de cobertura total.

O readback posterior do pai em OFF tem zero NaNs no prefixo examinado, embora o
upload inicial tivesse 139. Seu hash guest do mip seguinte permanece igual antes
e depois. Isso mostra que o prefixo base pode ter sido sobrescrito corretamente
na GPU e reforça por que o upload inicial inválido não prova corrupção final.
Não certifica os outros mips ou faces.

OFF termina com 45 eventos numerados sem end/budget_end; ON chega a 64/budget_end
e registra lost=2. A ausência de eventos fora desses limites não é ausência de
operações. Nenhum registro estabelece uma aresta de shader entre o cubemap e um
render target de iluminação do menu. A interpretação como ambiente/reflexo é
compatível com seis faces/mips, mas permanece inferência.

## Relação com os três casos de ec8e8b89

- Reuso de representação antiga: a promoção/fullMatch é o candidato específico
  observado; falta confrontar os níveis GPU dos filhos com os novos níveis do pai.
- Readback antigo sobrescrevendo guest novo: não foi demonstrado nestes logs;
  a amostra de 4096 bytes do readback OFF permanece igual antes/depois. Isso não
  exclui alterações fora da amostra.
- Produtor contido omitido: a seleção inicial registrada é fallback-candidate
  seguida de readback-request, não uma fonte simplesmente ausente da lista.
  Intenção de escrita pendente e intervalos de mip disjuntos não bastam para
  afirmar que um produtor GPU recuperável foi perdido nessa etapa.

## Menor evidência adicional necessária — proposta, não implementada

Não é necessário repetir os reprodutores, aumentar logs globais ou fazer outra
build agora. A próxima instrumentação, caso outra coleta seja necessária,
deve mirar **a primeira expansão 128×128 de um para oito mips** desta família:

1. Um registro de promoção com níveis antigos/novos e escritores-filhos observados
   por nível/camada, distinguindo intenção, ciclo submetido e cobertura conhecida.
2. Na preparação já existente de initialization, registrar o mip/camada, offsets
   guest/staging e alguns valores half correspondentes (mesmo texel), além do
   handle/região do copyBufferToImage. Incluir o primeiro texel NaN/excessivo do
   staging de upload normal com sua origem guest exata, em vez de comparar o hash
   físico de um mip ao prefixo linear de outro.
3. Uma aresta de consumo do pai expandido: view mip/layer, sampler LOD e shader/
   destino do draw, para verificar ligação real com a iluminação do menu.

Esses registros podem observar buffers/comandos já existentes, sem cópia GPU ou
espera adicional. Ainda assim, provar que um filho possui *bytes GPU diferentes*
exige observar seu conteúdo: aproveitar um readback existente se houver. Se não
houver tal readback/clear de valor conhecido, logs de serial sozinhos não podem
fornecer essa prova; uma eventual amostra GPU extra seria uma decisão separada,
não foi adicionada nem solicitada uma APK nesta etapa.

## Conclusão

Origem imediata dos NaNs: bits do caminho guest nativo chegam ao staging de upload;
origem anterior desses bits e sua relação com o menu continuam indeterminadas.
Zeros ON: compatíveis com descarte anterior bem-sucedido de outra representação
cujo span cobre os mesmos bytes — não é necessário supor o descarte local no futuro.
Falha de autoridade no jogo: candidata focalizada na expansão de mips, ainda sem
prova dos dados GPU efetivos e do consumidor. Não há correção de sequenciamento
ou afirmação de causa-raiz visual confirmada.

Evidência numérica e eventos GPU-PHYS extraídos, sem o restante dos logs pessoais:
`docs/checkpoints/physical-authority-device-content-20261122/`. Asserções de análise
validaram os offsets da cadeia, a disjunção inicial e as sete coberturas de descarte.
Não foram executados testes antigos nem builds Android.
