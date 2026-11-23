# Coerência por subrecurso — auditoria independente da #481

## Decisão e base

Base: master `8bcaee0be950b44234cdd2767a11578c74db2602`, obtida por fetch nesta
rodada. Branch independente `audit/subresource-coherence-20261122`.
A #481 permanece em 537f8b4e, sem modificações. Este trabalho preserva um novo
reprodutor e controles de transferência, mas **não integra uma política incompleta
no renderer**. Não é correção do SpongeBob. Não foi criada PR de correção, APK,
instrumentação de jogo ou merge.

O defeito específico da expansão foi reproduzido na master com Vulkan real.
Uma cópia exata corrige o caso quando o teste fornece a autoridade conhecida.
A seleção dessa autoridade e sua manutenção depois da promoção continuam sem
uma implementação segura para integrar. A etapa seguinte exigiria alterar os
contratos de escrita/invalidação/registro, não apenas RecordBackingPromotion.
Conforme o limite de escopo, a auditoria é encerrada antes dessa migração.

## Fonte Ryujinx e licença

Referência solicitada lida no código real:
https://github.com/Stella-sea/ryujinx-admin/tree/7de2b673e1295ba3de017afab965f79cc6b9bb66
(branch canary, checkout 7de2b673e1295ba3de017afab965f79cc6b9bb66).
LICENSE.txt: MIT, copyright Ryujinx Team and Contributors. Não foi transcrito
código Ryujinx para o Strato ou para os testes. Os novos controles usam o fixture
MPL-2.0 existente do Strato; adaptam conceitos, sem copiar a implementação C#.

| Mecanismo na referência, caminhos em src/Ryujinx.Graphics.Gpu/Image/ | Semântica relevante | Strato master |
| --- | --- | --- |
| TextureCache.cs::FindOrCreateTexture, overload TextureSearchFlags/TextureInfo | Classifica compatibilidade física/layout; inicializa backing novo antes de copiar overlaps; compartilha backing com ReplaceView quando Full, registra dependências quando CopyOnly | Reusa views compatíveis e tem expansão preservando mips existentes; fullMatch pode crescer antes de recuperar matches independentes |
| TextureGroup.cs::CreateCopyDependency(Texture,int,int,bool) | EnsureFullSubdivision, percorre handles correspondentes de mip/camada; registra relações e cópia inicial por origem válida | AliasGroup para certas representações reversíveis, sem dependência geral de filhos nos níveis adicionados |
| TextureGroupHandle.cs::SignalModified e TextureDependency.cs::SignalModified | Modified + notificação recíproca de cópia diferida + sync tracking | RecordGpuWrite marca serial da Texture e invalida somente membros do AliasGroup |
| TextureGroupHandle.cs::Copy | DeferredCopy, CheckDirty, Modified e bind count controlam origem/repetição; CopyTo passa mip/camada | Promoção usa GPU do próprio pai e guest para níveis novos, sem escolher outras origens GPU |
| TextureGroup.cs::SynchronizeMemory | Resolve NeedsCopy, preserva GPU Modified; SynchronizePartial quando somente partes precisam upload | SynchronizeHostImpl materializa a textura descrita; dirtyState é por imagem |
| TextureGroupHandle.cs::DirtyAction/Dispose/Inherit | Escrita CPU cancela DeferredCopy; dependências sobrevivem à troca de storage e são retiradas na destruição | Traps/gerações por Texture e grupos existentes; não há equivalente completo para dependências físicas parciais |

Modified no Ryujinx **não significa clear completo**. Uma escrita parcial pode
ser representada por autoridade sobre o subrecurso inteiro se, antes da escrita,
a representação já continha uma cópia atual de todos os bytes preservados e todas
as outras escritas invalidam seus dependentes. Copiar apenas o flag/serial para o
Strato sem garantir essa pré-condição perderia dados.

## O que já existe e não deve ser duplicado

- TextureManager::FindOrCreate identifica matches de mip/camada, compartilha views,
  registra certos aliases e usa exceções para recuperação fora dos locks do draw.
- RequireAliasCoherence/ReconcileAliases já invalidam e restauram grupos conhecidos;
  não há benefício em criar outro grafo paralelo para esses mesmos grupos.
- CanGrowMipLevels valida formato, layout dos níveis existentes, stride, capacidade,
  memoryFreed. Validações de geometria não são validações de autoridade.
- PromoteBackingForView preserva GPU dos mips antigos e lifetime de views/backings;
  SynchronizeHost(false) trata CPU-dirty do próprio pai. Os novos níveis são lidos
  do mirror guest; outros escritores não participam da escolha de origem.
- RecordBackingPromotion já contém as transições e cópias. Acrescentar uma barreira
  não materializa dados de outro VkImage nem resolve qual imagem está atual.
- O executor submete/reprepara no pedido de promoção; mantém ciclos por Texture.
  Dependência de execução e dependência de conteúdo são invariantes diferentes.
- SynchronizeGuest publica o layout da imagem; o problema de readback obsoleto
  parcial de ec8e8b89 não se resolve ordenando imagens pelo serial global.
- FreeGuest e pins já controlam disponibilidade de guest, não autoridade entre
  backings. O descarte/GC não pode criar nem apagar a única origem válida.

Consulta #400: checkpoint a448ad06e84f0edd193520c08d17e5d1b819e56f,
texture.cpp regionAuthority/ReconcileRegions e mixed_region_authority.inc:
partições, registro transitivo de membros, locks ordenados e revalidação após
bloqueio demonstram por que importar só a cópia ou o serial seria incompleto.
Nenhum desses módulos foi importado. Reutilizar esse subsistema inteiro excederia
a adaptação incremental solicitada e seus limites já documentados.

Os três reprodutores ec8e8b89 (cache antigo, readback que sobrescreve autoridade
parcial, representação contida omitida) foram consultados no relatório e no código
preservado na #481. Não foram reexecutados; não se reivindicam como descobertas novas.

## Novo experimento determinístico, antes de integrar renderer

`tests/gpu_backing_promotion/subresource_growth_cases.h`, modo
`TEXMAN_SUBRESOURCE_ONLY=1` do harness existente. Extrai as funções **da master
atual**, sem substituir PromoteBackingForView/RecordBackingPromotion por modelos.

Pai RGBA16F 128×128, um mip, seis faces, stride guest 180224. Sete imagens-filhas
64→1 compartilham os payloads físicos exatos dos mips correspondentes. Cada face
recebe bits half conhecidos e distintos por mip/camada via comandos GPU. Guest
continua zero, exceto o controle CPU posterior explicitado abaixo. Os writes GPU
são operações reais, não apenas incremento do serial.

A expansão para oito mips preserva o mip0, mas inicializa mips1–7 pelos bytes guest.
Após readback Vulkan concluído do pai, os 42 subrecursos não contêm os dados GPU
que ficaram nos filhos. O último produtor foi submetido sem Wait CPU antes da
expansão; a mesma fila Vulkan ordena os comandos. Antes da comparação/cópia de
controle todas as operações produtoras efetivamente concluíram. A perda não é
resolvida apenas aguardando fences.

Controles positivos, exclusivamente no teste:

- Cópia GPU→GPU de cada mip correspondente e seis camadas, sem alterar mip0.
- No caso completo, comparar todos os bytes de cada subrecurso do pai com o
  staging de origem conhecido depois da cópia e fence.
- No caso parcial, somente (0,0) de cada face foi escrito na GPU. Guest recebe
  depois um pixel CPU distinto em (1,0), mip1/camada0. Copiar o retângulo conhecido
  preserva ambos. Readback do filho confirma que seu segundo pixel é antigo/zero;
  portanto uma cópia integral desse filho apagaria o pixel CPU mais recente.
- View do mip0 criada antes da promoção é reutilizada depois com handle atualizado;
  identidade do mapeamento guest é preservada.

A atualização CPU deste controle é uma escrita direta no mirror do fixture: testa
a mistura dos bytes na expansão, **não** os callbacks concorrentes do TrapManager
Android. Os retângulos conhecidos são fornecidos pelo teste, não inferidos de
RecordGpuWrite. Não há planner de autoridade integrado. O controle positivo não é
uma implementação corrigida do renderer nem prova de correção após futuras escritas.

## Cobertura e resultados

Oito combinações: flag Free Guest OFF/ON × writes completos/parciais × suporte à
lista de formatos Vulkan OFF/ON. ASan/UBSan, Vulkan Validation + synchronization
validation: oito casos passaram, zero erros. LeakSanitizer desativado como nos
harnesses anteriores; não se afirma ausência de leaks Android.

| Caso solicitado | Resultado desta rodada |
| --- | --- |
| Filho GPU antes da expansão | Reproduzido: pai perde os bytes; controle exato preserva |
| Múltiplos mips | Sete filhos/mips diferentes em cada execução |
| Seis faces independentes | 42 payloads diferenciados, comparados após conclusão |
| Writes parciais | Um texel/face, somente região escrita copiada no controle |
| CPU posterior | Segundo pixel guest mais novo preservado pela cópia parcial; filho inteiro seria incorreto; trapping real não testado |
| Spans sobrepostos, payloads disjuntos | Mips com stride compartilhado, mip0 intacto; não confundir span com escrita |
| Submissões pendentes | Último ciclo submetido, sem Wait CPU antes da expansão; gravação concorrente pré-submit não simulada |
| Views/aliases após reconciliação | View base reutilizada após promoção; aliases independentes após futuras escritas continuam bloqueio |
| Free Guest ON/OFF | Ambos flags testados com pins; liberação real/recuperação não certificadas por esses casos |
| GC/descarte | Lifetime de cópias via dependências do fixture; GC real não exercitado e não validado |

O teste falha se exigir que o pai original já contenha os filhos, e passa com a
assertiva explícita de falha conhecida mais o controle de cópia. Não é red→green
de patch de produção: a falha do renderer continua presente e documentada.

O harness antigo da master inicialmente não compilou por ausência de sparseSegments
/ ReadSparseBacking na facade. Foi atualizado apenas o contrato do fixture: campo
sparseSegments e um ReadSparseBacking que lança se usado. Esta suíte contém apenas
mapeamentos nativos residentes e não chama esse stub. Testes sparse existentes não
foram substituídos por esse controle. Uma assertiva inicial do controle comparava
o segundo pixel ao staging inteiro, que não foi copiado nesse retângulo; foi
corrigida para ler o conteúdo GPU real do filho. O resultado final usa essa leitura.

Execução reproduzível, com Vulkan-Hpp/Headers, Boost, loader, ICD e layers instalados:

```sh
TEXMAN_SUBRESOURCE_ONLY=1 CXXFLAGS="-fsanitize=address,undefined" \
  ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_backing_promotion/run.py
```

Não foram executadas as suítes antigas ec8/#480. Evidência final:
`docs/checkpoints/subresource-coherence-20261122/asan-ubsan-vulkan.log`.

## Modelo avaliado e motivos para não integrar ainda

O modelo adequado é **réplica válida por subrecurso**, com invalidação e cópia
preguiçosa. Identidade precisa representar a sequência física exata de bytes de
cada mip/camada, formato nativo e layout block-linear, sem tratar padding ou
spans inteiros como escrita. Cada registro precisa conhecer réplicas válidas,
gerador da versão, origem guest válida e ciclos/lifetime associados.

Invariantes necessários para uma implementação incremental:

1. Antes de uma escrita GPU parcial, o destino já é uma réplica atual dos bytes
   preservados. Depois, os pares sobrepostos são invalidados. Sem isso um flag
   Modified não certifica autoridade sobre o mip inteiro.
2. Escrita CPU cancela a cópia diferida exatamente para as regiões afetadas, após
   preservar dados GPU necessários. Readback só publica as regiões ainda válidas
   de sua versão; não pode reviver bytes superseded de outro mip/representação.
3. Registro de um novo pai/filho herda o histórico existente e mantém dependências
   em ambos sentidos, inclusive futuros cache hits, buffers/DMA e storage writes.
4. Se layouts não permitem definir uma unidade comum de autoridade, marcar origem
   como desconhecida e manter o fallback atual, sem inferir validade pelo serial.
5. Promoção muda réplicas/backing generation sem perder as versões; GC/remap retira
   réplicas somente quando não destrói a última origem e há conclusão de seus usos.

Uma primeira fatia viável poderia aceitar apenas correspondências exatas nativas
mip/camada e componentes sem overlaps incompatíveis. Ainda assim não basta varrer
filhos no momento da expansão: é necessário registrar os eventos anteriores e
invalidar todas as réplicas em writes posteriores. As APIs atuais RecordGpuWrite()
e MarkGpuDirty não recebem view/mip/camada/retângulo nem um certificado de réplica
atual. Os traps invalidam objetos/grupos, e não há uma versão física parcial que
prove simultaneamente as condições acima. Um scan sem esse contrato trataria
intenção, estado GpuDirty e serial como prova de autoridade — precisamente o erro
que a solicitação proíbe.

Não foi adotado um patch que copia filhos inteiros, cria um segundo grafo paralelo
aos aliases ou acrescenta waits/readbacks. A migração mínima exigiria contrato de
subresource use para todos os produtores + invalidação CPU + integração de aliases,
readback e retirada de backing. Isso cruza mais caminhos do que uma correção local
de expansão e merece projeto/testes próprios antes de produção.

## Custo e limites finais

Renderer não modificado: zero cópias/waits/allocations novos em produção por esta
branch; nenhum aumento em FindOrCreate. Isso não é benchmark de uma solução ainda
não implementada e não afirma FPS/frametime Android.

O controle de teste usa sete comandos copyImage para 42 subrecursos: 262128 bytes
no caso completo, 336 bytes no caso parcial. Readbacks e esperas são exclusivos do
harness para comparação. Nenhum dado disso mede overhead no Moto G100. CPU/RAM/GC
de uma futura dependência por subrecurso não foram medidos.

Falha nova comprovada: inicialização guest dos novos mips perde os dados dos filhos
no caso reduzido. Solução mecânica comprovada sob autoridade conhecida: cópia exata
GPU→GPU. Bloqueio da integração: certificar e manter aquela autoridade no renderer
sob writes parciais/CPU/aliases/GC. Os demais defeitos conhecidos permanecem, sem
uma nova alegação de correção. Não há PR de implementação ou APK a validar.
