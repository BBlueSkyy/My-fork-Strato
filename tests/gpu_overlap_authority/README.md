# Autoridade em sobreposição parcial: regressão local

Base: PR #392 (`7c42375a`). O teste compila os métodos de produção do checkout
com Vulkan real e verifica a coerência por interseção, sem APK.

## Executar

Requer Linux, Python 3, Clang C++20 com UBSan, Boost headers, Vulkan-Hpp,
Vulkan loader, Mesa lavapipe e `VK_LAYER_KHRONOS_validation`.

```bash
CXX=clang++ \
VULKAN_HPP_INCLUDE_DIR=/usr/include \
VULKAN_HEADERS_INCLUDE_DIR=/usr/include \
python3 tests/gpu_overlap_authority/run.py --check-correct
```

Selecione lavapipe com `VK_ICD_FILENAMES` se houver outros dispositivos.
`BOOST_INCLUDE_DIR`, `VULKAN_LOADER_LIBRARY`, `VK_LAYER_PATH` e `CXXFLAGS`
permitem dependências locais. A compilação usa UBSan e habilita validação de
sincronização Vulkan. Ambos os modos, padrão e `--check-correct`, exigem
coerência correta e retornam zero somente se todas as verificações passarem.
Nenhuma assertion exige a corrupção anterior.

## Componentes e limites

O runner extrai corpos completos do Texture Manager, registro/reconciliação,
sincronização, traps, promoção e swizzlers do checkout. Reutiliza as facades
Vulkan de `tests/gpu_backing_promotion`, cujo main não é executado.
`IntervalMap` e `TrapManager` são os de produção, retirando apenas includes
Android. O observador registra a ordem dos callbacks sem impô-la.

Guest e mirrors usam mapeamentos compartilhados de um `memfd`; `mprotect` é
real. Imagens, render passes, staging, comandos, submissões e fences também
são reais. O harness substitui allocator/scheduler/fence, construção/lifetime
de Texture e locks de contexto. O signal handler ARM64/NCE não é executado:
`HandleTrap` é chamado explicitamente, incluindo uma leitura sem store após
um callback write, porque NCE pode encaminhar leituras dessa forma.
Há também um teste separado com store CPU real.

Os recursos permanecem vivos e suas alocações são conferidas; a fixture não
confunde invalidação de uma representação com reutilização de memória.
O consumo GPU é observado por cópia Vulkan da imagem, sem shader sampling.
O formato é RGBA8 nativo. Estes testes não demonstram correção visual de UE4
no aparelho nem medem FPS Android.

## Oráculo e cobertura

A/B principais são 64x32 RGBA8 block-linear, 8192 bytes cada, deslocadas em
4096 bytes. Cada região exclusiva e a interseção contém 1024 texels reais.
A escreve vermelho e opcionalmente magenta mais recente, ainda só na GPU;
B escreve verde depois. O resultado exigido é:

| Região | Valor |
| --- | --- |
| A exclusiva | magenta se A voltou a escrever; vermelho no controle |
| A ∩ B | verde |
| B exclusiva | verde |

São conferidos os bytes completos e páginas sentinela, o backing reutilizado,
a ordem dos callbacks e as contagens de submissões/alocações. A cobertura inclui:

- trap antes/depois do reuso, A Clean/GpuDirty, B pendente/já Clean e traps read/write;
- A→B→A com escrita parcial, preservando a interseção e as regiões exclusivas;
- texels versus padding, múltiplas camadas e mappings fragmentados;
- escritores inteiramente supersedidos sem readbacks antigos desnecessários;
- aliases de mapping idêntico dentro do componente parcial, inclusive registro
  posterior à supersessão total de A por B;
- retry real do manager/executor, inclusive GPU write ainda não submetida;
- readback/upload com falha e retry, callback sob contenção sem bloquear;
- crescimento de mips que passa a alcançar bytes escritos por B;
- rearmamento de traps, store CPU, supressão de descarte de bytes compartilhados;
- reusos estáveis sem submissões ou alocações extras.
