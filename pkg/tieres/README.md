# Pacote RetroArch TIERES

Arquivos que vão junto com o `retroarch.exe` nas versões TIERES, além do código.
Ficam aqui abertos (não em `.zip`) para dar para ver o diff de cada ajuste.

```
pkg/tieres/
  empacotar.ps1          gera os zips (versão completa, light e pacotes de gráficos)
  comum/                 vai em TODA versão e em todos os pacotes de gráficos
    config/PCSX-ReARMed/PCSX-ReARMed.opt     opções do núcleo do PlayStation
  graficos/
    Casanova/            PADRÃO: vai dentro de toda versão (completa e light)
    PS1/                 pacote opcional: SMAA+FSR e outros shaders
```

## Gráficos

- **Casanova** (padrão): Resolução Aprimorada, bilinear, sem shaders, tela cheia na
  resolução do monitor. Leve. É o que toda versão nova já traz.
- **PS1** (opcional): mesmo `.opt`, mais o preset `SMAA+FSR` e mais 4 shaders para
  escolher no menu. Os shaders vieram do [libretro/slang-shaders](https://github.com/libretro/slang-shaders);
  o `SMAA.hlsl` tem um ajuste nosso (procure `TIERES:` no arquivo).

Os dois também são distribuídos soltos (`RetroArch-TIERES-Graficos-*.zip`), para quem
quer trocar de um para o outro sem baixar a versão de novo. As instruções para o
jogador estão no `!LEIA-ME-Graficos-*.txt` de cada pasta.

Quem tinha o pacote PS1 e atualizar com a versão light volta para o Casanova
(o `.cfg` do light desliga os shaders). É só extrair o zip do PS1 de novo.

Para trocar o padrão, mude `$GraficoPadrao` no `empacotar.ps1`.

## Gerar uma versão

1. Atualize `version.all` e compile.
2. Monte a pasta da versão (`RetroArch-1.16.0.FFW.TIERES.0.x`) com o `retroarch.exe`,
   o `kailleraclient.dll` e o `cores\pcsx_rearmed_libretro.dll` novos.
3. Rode:

   ```powershell
   .\pkg\tieres\empacotar.ps1 -Pasta D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.update\RetroArch-1.16.0.FFW.TIERES.0.x
   ```

   O script copia `comum\` e o `config\` do Casanova para dentro da pasta (e apaga o
   `.slangp` do PS1, se sobrou de algum teste) e gera, ao lado dela:

   - `RetroArch-1.16.0.FFW.TIERES.0.x.zip` (completo; pule com `-SemCompleto`)
   - `RetroArch-1.16.0.FFW.TIERES.0.x.light.zip`
   - `RetroArch-TIERES-Graficos-Casanova.zip` e `RetroArch-TIERES-Graficos-PS1.zip`

   Se algum zip já existir, ele para; use `-Substituir` para sobrescrever.
   Só os pacotes de gráficos: `.\pkg\tieres\empacotar.ps1 -Saida <pasta>`.

## Observações

- O `.gitignore` da raiz ignora pastas `config`; o `.gitignore` daqui as libera.
- O `.gitattributes` daqui desliga a conversão de fim de linha: os arquivos vão para
  os zips exatamente como estão (LEIA-ME em CRLF com BOM, `.cfg`/`.opt` em LF).
