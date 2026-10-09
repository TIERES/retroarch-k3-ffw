<#
.SYNOPSIS
  Gera os zips de uma versao do RetroArch TIERES, com o grafico padrao (PC Antigo) ja aplicado.

.DESCRIPTION
  Sempre gera, em -Saida, um zip para cada pasta de graficos\:
    RetroArch-TIERES-Graficos-PC-Antigo.zip
    RetroArch-TIERES-Graficos-PC-Moderno.zip

  Com -Pasta (a pasta da versao, a mesma do retroarch.exe), tambem:
    1. copia comum\, versao\ e graficos\<padrao>\config\ para dentro da pasta, e apaga
       de la os arquivos de config dos outros pacotes de graficos (ex.: o .slangp do PC Moderno);
    2. gera <pasta>.light.zip, so com os binarios (quem atualiza mantem controles, graficos e
       opcoes do nucleo);
    3. gera <pasta>.zip (pule com -SemCompleto).

.EXAMPLE
  .\empacotar.ps1 -Pasta D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.update\RetroArch-1.16.0.FFW.TIERES.0.5

.EXAMPLE
  .\empacotar.ps1 -Saida D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.update
  (so os zips de graficos)
#>
param(
    [string]$Pasta,
    [string]$Saida,
    [switch]$SemCompleto,
    [switch]$Substituir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Grafico que vai dentro de toda versao completa.
$GraficoPadrao = 'PC-Antigo'

# A versao light so leva os binarios: ela vai por cima de uma pasta TIERES que ja tem o
# retroarch.cfg, o grafico e as opcoes do nucleo que o jogador escolheu. As opcoes que
# afetam a sincronia o retroarch.exe forca nas partidas (ksync_forced_options).
$LightObrigatorios = 'retroarch.exe', 'kailleraclient.dll', 'cores/pcsx_rearmed_libretro.dll'
# Trava do nucleo: impede o Online Updater de trocar o nucleo TIERES pelo oficial.
$LightOpcionais    = 'cores/pcsx_rearmed_libretro.dll.lck'

$Raiz     = $PSScriptRoot
$Comum    = Join-Path $Raiz 'comum'     # versao + pacotes de graficos
$Versao   = Join-Path $Raiz 'versao'    # so versao (completa e light)
$Graficos = Join-Path $Raiz 'graficos'

# Arquivos de $Dir como @{ Origem; Nome }, com Nome relativo usando '/'.
function Get-Itens([string]$Dir, [string]$Prefixo = '') {
    $base = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $Dir -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
        @{ Origem = $_.FullName; Nome = $Prefixo + $_.FullName.Substring($base.Length).Replace('\', '/') }
    }
}

function New-Zip([string]$Destino, $Itens, [string[]]$Pastas = @()) {
    $repetidos = $Itens | Group-Object { $_.Nome } | Where-Object Count -gt 1
    if ($repetidos) { throw "Arquivo repetido no zip ${Destino}: $($repetidos[0].Name)" }
    if (Test-Path -LiteralPath $Destino) {
        if (-not $Substituir) { throw "Ja existe: $Destino (use -Substituir para sobrescrever)" }
        Remove-Item -LiteralPath $Destino
    }
    $zip = [System.IO.Compression.ZipFile]::Open($Destino, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($p in $Pastas) { [void]$zip.CreateEntry($p) }
        foreach ($i in $Itens) {
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $i.Origem, $i.Nome, [System.IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $zip.Dispose()
    }
    Write-Host "  $Destino"
}

# --- validacao (nada e escrito antes daqui) ---------------------------------

$pacotes = @(Get-ChildItem -LiteralPath $Graficos -Directory | Sort-Object Name)
if (-not ($pacotes.Name -contains $GraficoPadrao)) { throw "Grafico padrao nao encontrado: graficos\$GraficoPadrao" }
$padraoConfig = Join-Path $Graficos "$GraficoPadrao\config"

# O RetroArch regrava o .cfg ao fechar com caminhos da maquina (ex.: cache_directory
# no TEMP do usuario). Na pasta do jogador eles nao existem; use :\ (pasta do retroarch.exe).
$absolutos = @(Get-ChildItem -LiteralPath $Raiz -Recurse -File | Where-Object Extension -in '.cfg', '.opt' |
    Select-String -Pattern '= "[A-Za-z]:[\\/]')
if ($absolutos) { throw "Caminho absoluto em $($absolutos[0].Path):$($absolutos[0].LineNumber): $($absolutos[0].Line)" }

if ($Pasta) {
    $Pasta = (Resolve-Path -LiteralPath $Pasta).Path.TrimEnd('\')
    $nome  = Split-Path $Pasta -Leaf
    if (-not $Saida) { $Saida = Split-Path $Pasta -Parent }

    foreach ($f in $LightObrigatorios) {
        if (-not (Test-Path -LiteralPath (Join-Path $Pasta $f))) { throw "Falta na pasta da versao: $f" }
    }

    $numero = (Select-String -LiteralPath (Join-Path $Raiz '..\..\version.all') -Pattern 'RARCH_VERSION="(.+)"').Matches[0].Groups[1].Value
    $esperado = 'RetroArch-' + ($numero -replace '\.K\.', '.')
    if ($nome -ne $esperado) { Write-Warning "Pasta '$nome' nao bate com version.all ($numero -> '$esperado')." }
}
if (-not $Saida) { throw 'Informe -Pasta (versao completa) ou -Saida (so os graficos).' }
$Saida = (Resolve-Path -LiteralPath $Saida).Path

# --- zips de graficos -------------------------------------------------------

Write-Host 'Graficos:'
foreach ($p in $pacotes) {
    $itens = @(Get-Itens $p.FullName) + @(Get-Itens $Comum)
    New-Zip (Join-Path $Saida "RetroArch-TIERES-Graficos-$($p.Name).zip") $itens
}

if (-not $Pasta) { return }

# --- aplica o padrao na pasta da versao -------------------------------------

Write-Host "Aplicando graficos '$GraficoPadrao' em ${Pasta}:"
$padrao = @(Get-Itens $Comum) + @(Get-Itens $Versao) + @(Get-Itens $padraoConfig 'config/')
foreach ($i in $padrao) {
    $alvo   = Join-Path $Pasta $i.Nome.Replace('/', '\')
    $estado = 'novo'
    if (Test-Path -LiteralPath $alvo) {
        $estado = if ((Get-FileHash -LiteralPath $alvo).Hash -eq (Get-FileHash -LiteralPath $i.Origem).Hash) { 'igual' } else { 'substituido' }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $alvo) | Out-Null
    Copy-Item -LiteralPath $i.Origem -Destination $alvo -Force
    Write-Host "  $($i.Nome) ($estado)"
}

# Sobras de outro pacote de graficos testado nesta pasta (ex.: PCSX-ReARMed.slangp do PC Moderno).
foreach ($p in $pacotes | Where-Object Name -ne $GraficoPadrao) {
    $cfg = Join-Path $p.FullName 'config'
    if (-not (Test-Path -LiteralPath $cfg)) { continue }
    foreach ($i in Get-Itens $cfg 'config/') {
        if ($padrao.Nome -contains $i.Nome) { continue }
        $alvo = Join-Path $Pasta $i.Nome.Replace('/', '\')
        if (Test-Path -LiteralPath $alvo) {
            Remove-Item -LiteralPath $alvo
            Write-Host "  removido $($i.Nome) (pacote $($p.Name))"
        }
    }
}
if (Test-Path -LiteralPath (Join-Path $Pasta 'shaders\shaders_slang')) {
    Write-Warning 'A pasta da versao tem shaders\shaders_slang (sobra do pacote PC Moderno?). Ela vai para o zip completo.'
}

# --- light e completo -------------------------------------------------------

Write-Host 'Versao:'
$light = @($LightObrigatorios) + @($LightOpcionais | Where-Object { Test-Path -LiteralPath (Join-Path $Pasta $_) })
New-Zip (Join-Path $Saida "$nome.light.zip") @($light | ForEach-Object { @{ Origem = (Join-Path $Pasta $_.Replace('/', '\')); Nome = $_ } })

if (-not $SemCompleto) {
    $base   = $Pasta + '\'
    $pastas = @("$nome/") + @(Get-ChildItem -LiteralPath $Pasta -Recurse -Directory -Force | Sort-Object FullName |
        ForEach-Object { "$nome/" + $_.FullName.Substring($base.Length).Replace('\', '/') + '/' })
    New-Zip (Join-Path $Saida "$nome.zip") @(Get-Itens $Pasta "$nome/") $pastas
}
