# Progresso dos controles de toque

Branch: `codex/import-claude-touch-controls-20261103`.
As caixas de verificação de aparelho/build só serão encerradas com resultado
real ou limitação específica registrada em `touch-controls.md`.

- [x] Ler contrato, documentação e conferir estado recebido.
- [x] 1. Extrair lógica pura, integrar produção e testar no host (7.675 checks).
- [x] 2. Opções de mira/ataque e gestos de alta densidade.
- [x] 3. Bloco único, mão ativa e mineração simultânea ao movimento.
- [x] 4. Opacidade, presets, perfis, restauração e avisos do editor.
- [x] 5. Gestos de UI, alvos ampliados, janelas, teclado e área segura (aparelho pendente; limite documentado).
- [x] 6. Cancelamento/multitouch e regressões automatizadas.
- [x] 7. Auditar custo, reaproveitar buffers e cachear rótulos (20.000 ciclos puros: zero alocações).
- [x] 8. Documentação de uso, limites e 18 testes manuais no Redmi.
- [x] Compilar testes de toque e gamepad com `-Werror`; saídas no relatório.
- [x] Verificar cada C++ alterado nas variantes aplicáveis; 44 checks, zero erros/avisos novos.
- [x] Verificar whitespace, ausência de feedback tátil novo e compatibilidade.
- [x] Verificar pré-requisitos e possibilidade de APK; falha de configure documentada.
- [x] Publicar commits pequenos na branch e atualizar PR.

Verificação final concluída: duas matrizes de 22 checks com NDK r26d arm64/API 28
real, Windows launcher e Windows clássico. Uma usa headers da engine; outra,
os tags solicitados de fmt 10.2.1 e xxHash 0.8.2. Nenhum erro ou aviso novo.
Os testes puros passaram em 7.675 checks de toque e 31.695 de gamepad.
Evidências, arquivos e comandos em `touch-controls-validation.md`.

Tentativa de APK: CMake + Ninja + vcpkg + NDK instalados no workspace.
O configure do preset arm64 falhou porque o triplet de ferramentas do host
`x64-windows` exige Visual Studio e não há instância completa instalada.
Nenhum APK foi produzido; a limitação está registrada no documento final.

Publicação confirmada no [PR #3](https://github.com/Cleitin790/oSBM/pull/3),
aberto para revisão e sem merge. As árvores de cada commit publicado foram
comparadas por SHA com as árvores locais. O relatório final registra a falha
de configure e os limites de aparelho, teclado, HUD real e plataformas não testadas.
As caixas acima indicam implementação/verificação disponível ou limite documentado;
não significam que os testes manuais em aparelho tenham sido executados.
