# Pacote RetroArch TIERES

Arquivos que vão junto com o `retroarch.exe` nas versões TIERES, além do código.
Ficam aqui abertos (não em `.zip`) para dar para ver o diff de cada ajuste.

```
pkg/tieres/
  empacotar.ps1          gera os zips (versão completa, light e pacotes de gráficos)
  comum/                 vai em toda versão completa e em todos os pacotes de gráficos
    config/PCSX-ReARMed/PCSX-ReARMed.opt     opções do núcleo do PlayStation
  versao/                vai em toda versão completa, mas não nos gráficos
    retroarch.cfg                            configuração geral (controles, Kaillera, menu...)
  graficos/
    PC-Antigo/           PADRÃO: vai dentro de toda versão completa (VGA onboard)
    PC-Moderno/          pacote opcional: SMAA+FSR e outros shaders (VGA dedicada)
```

A versão light não leva nada disso, só os binários (`retroarch.exe`, `kailleraclient.dll`,
o núcleo e a trava dele). Ela vai por cima de uma pasta TIERES que já tem o `retroarch.cfg`,
o gráfico e as opções do núcleo que o jogador escolheu, e não pode desfazer nada disso.

## Gráficos

- **PC Antigo (VGA onboard)** (padrão): Resolução Aprimorada, bilinear, sem shaders, tela
  cheia na resolução do monitor. Leve. É o que toda versão completa já traz. Até
  2026-10-09 se chamava "Casanova".
- **PC Moderno (VGA dedicada)** (opcional): mesmo `.opt`, mais o preset `SMAA+FSR` e mais 4 shaders para
  escolher no menu. Os shaders vieram do [libretro/slang-shaders](https://github.com/libretro/slang-shaders);
  o `SMAA.hlsl` tem um ajuste nosso (procure `TIERES:` no arquivo). Até 2026-10-09 se
  chamava "PS1".

Os dois também são distribuídos soltos (`RetroArch-TIERES-Graficos-*.zip`), para quem
quer trocar de um para o outro sem baixar a versão de novo. As instruções para o
jogador estão no `!LEIA-ME-Graficos-*.txt` de cada pasta. No site eles ficam no card
"Pack de Gráficos".

Para trocar o padrão, mude `$GraficoPadrao` no `empacotar.ps1`.

## retroarch.cfg

O `versao/retroarch.cfg` é o que vai para os jogadores. Ele fica fora dos pacotes de
gráficos para que instalar um gráfico não mexa nos controles de ninguém.

Ele também fica fora da versão light (veja acima). Por isso, uma novidade de uma versão
nova não pode depender de mudança no `retroarch.cfg` nem no `.opt`: o exe tem que aplicar
sozinho, como o chat no TAB da 0.5 (`kailleraChatKeyTab()`) e as opções de sincronia
(`ksync_forced_options`).

Para mudar uma configuração, edite este arquivo. O `retroarch.cfg` da pasta da versão é
sobrescrito pelo script, e o RetroArch regrava o arquivo ao fechar.

Se copiar para cá um `retroarch.cfg` salvo pelo RetroArch, tire os caminhos da sua
máquina. Os caminhos usam `:\` (pasta do `retroarch.exe`); o que não tiver no arquivo fica
no padrão do RetroArch. O script recusa `.cfg`/`.opt` com caminho absoluto (`C:\...`).
Foi assim que um `cache_directory` apontando para o TEMP do usuário de quem gerou a
versão foi parar nas versões até a 0.4. No PC dos jogadores essa pasta não existe, e é nela
que o RetroArch extrai jogos compactados. A linha foi removida, e cada jogador volta a usar
o próprio `%TMP%`.

## Gerar uma versão

1. Atualize `version.all` e compile.
2. Monte a pasta da versão (`RetroArch-1.16.0.FFW.TIERES.0.x`) com o `retroarch.exe`,
   o `kailleraclient.dll` e o `cores\pcsx_rearmed_libretro.dll` novos.
3. Rode:

   ```powershell
   .\pkg\tieres\empacotar.ps1 -Pasta D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.update\RetroArch-1.16.0.FFW.TIERES.0.x
   ```

   O script copia `comum\`, `versao\` e o `config\` do PC Antigo para dentro da pasta
   (e apaga o `.slangp` do PC Moderno, se sobrou de algum teste) e gera, ao lado dela:

   - `RetroArch-1.16.0.FFW.TIERES.0.x.zip` (completo; pule com `-SemCompleto`)
   - `RetroArch-1.16.0.FFW.TIERES.0.x.light.zip` (só os binários)
   - `RetroArch-TIERES-Graficos-PC-Antigo.zip` e `RetroArch-TIERES-Graficos-PC-Moderno.zip`

   Se algum zip já existir, ele para; use `-Substituir` para sobrescrever.
   Só os pacotes de gráficos: `.\pkg\tieres\empacotar.ps1 -Saida <pasta>`.

## Observações

- O `.gitignore` da raiz ignora pastas `config`; o `.gitignore` daqui as libera.
- O `.gitattributes` daqui desliga a conversão de fim de linha: os arquivos vão para
  os zips exatamente como estão (LEIA-ME em CRLF com BOM, `.cfg`/`.opt` em LF).
