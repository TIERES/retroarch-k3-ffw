# rollback_harness

Roda o `kaillera/kaillera_sync.c` de verdade em dois "PCs" ao mesmo tempo, sem RetroArch,
sem core e sem servidor, para testar o detector de desync e a volta automática.

- `stubs.c` troca as ~40 dependências do arquivo (core, OSD, chat, arquivos, threads) por
  versões simples. O "core" é uma RAM de 2 MB que muda a cada frame conforme o input, e o
  savestate é essa RAM.
- O mesmo `ksync_h.dll` é carregado duas vezes, de pastas diferentes (`A\` e `B\`). Cada cópia
  tem as suas variáveis globais, ou seja, é um PC.
- `driver.c` faz o papel do `core_run()` do `runloop.c`, frame a frame, nos dois PCs:
  `FrameTick`, o servidor devolvendo input e comando de 3 frames antes, `FrameMode`
  (roda ou congela), `AfterFrame` e `OnCommand` para RESTORE/RESUME. As linhas `[SYNC]` chegam
  aos dois PCs 8 frames depois, porque o servidor ecoa o chat para quem mandou.
- O desync é provocado invertendo um byte da RAM só do PC B, como fazia a antiga tecla F10.
- A cada frame a RAM dos dois PCs é comparada, e o resultado diz em que frames elas ficaram
  diferentes.
- O log de cada PC fica em `A\logs\kaillera_sync.log` e `B\logs\kaillera_sync.log`.

## Como rodar

No MSYS2 MINGW64, com o fork já configurado e compilado (o script usa as mesmas flags de
compilação do `kaillera_sync.c`):

```
kaillera/tests/rollback_harness/build.sh /tmp/ksync_harness
cd /tmp/ksync_harness
./driver.exe 7200 1500,3000,4500,6000        # 4 desyncs: 3 voltas automáticas e depois o BACKSPACE
./driver.exe 4000 300                        # desync antes do 1º ponto de volta: só o aviso
./driver.exe 5000 1500,2100                  # 2º desync logo depois de voltar: volta ao ponto 0
./driver.exe 4000 1500 1800                  # o RESTORE se perde (outro comando no mesmo frame)
EXTRA_RESTORE=1900:1 ./driver.exe 4000 1500  # BACKSPACE do host na contagem: volta mais 10 s
```

Argumentos: `<frames> <frames dos desyncs> [frame a partir do qual o próximo RESTORE se perde]`.

## Resultado esperado (2026-11-23)

- Após cada volta, `RAM HOST x PEER diferente nos frames X-Y` termina no frame do RESUME.
  Depois disso as RAMs dos dois PCs ficam iguais até o próximo desync.
- Os dois PCs congelam no mesmo frame e pelo mesmo número de frames (183, ou seja, ~3 s).
- As mensagens, nos dois PCs:
  `Desync detectado! Voltando a partida em 10 segundos atrás...`, depois uma por segundo:
  `Voltando em 3...`, `Voltando em 2...` e `Voltando em 1...`. Em seguida,
  `Partida retornada a momentos antes do desync.`.
- No 4º desync da partida não há volta automática. Aparece o aviso para o host apertar
  BACKSPACE.
- Se o RESTORE se perder, o host manda outro 3 s depois.
