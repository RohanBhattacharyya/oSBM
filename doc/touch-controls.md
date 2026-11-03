# Controles de toque e interface móvel

Implementação na branch `codex/import-claude-touch-controls-20261103`, para revisão
no PR #3. Não há troca automática de esquema por arma, mineração ou construção.
Os perfis desta página são layouts visuais escolhidos no launcher. O gamepad
continua usando seu esquema universal e seus remapeamentos salvos.

## Layout e mineração

O layout inicial tem 18 elementos cadastrados e 13 habilitados: dois joysticks
e onze botões. O joystick esquerdo move; o direito mira. Permanecem os botões
L/R de uso das mãos, pulo, interação, pausa, chat, tecnologia e as rodas de
inventário e ações. Segure uma roda, arraste na direção da opção e solte;
soltar no centro cancela. Deslizar a hotbar percorre os slots. Mouse físico e
navegação de gamepad não iniciam esse gesto.

Os dois novos elementos entram por seus IDs no merge das configurações:

- `singleBlock`: botão **1 block**, alterna LShift para bloco único. O destaque
  de pressionado permanece enquanto o modificador está ativo. Tocar novamente
  desliga. Abrir janelas ou perder foco também libera o modificador.
- `swapAimHand`: botão **L/R**, troca a mão usada pelas opções de ataque do
  joystick de mira. Começa na esquerda. Os botões L e R originais continuam
  independentes. A mão selecionada vale para a sessão, não altera remapeamentos.

É possível segurar mineração com um dedo enquanto o outro move. A liberação do
joystick de movimento não envia clique quando há um ataque direto em andamento.
Nenhuma regra de alcance, dano, ferramenta, colocação ou remoção foi modificada.
A opção preexistente **Precise aim** move um cursor livre; desativada, a mira
aponta ao redor do personagem. Não foi criado novo acionamento de R3.

## Opções no launcher

Em **Controls/Overlay**:

| Opção | Valor inicial / comportamento |
|---|---|
| Aim joystick action | **Aim only**, preserva o acionamento manual |
| Release to attack | Um pulso de 55 ms ao soltar fora da zona morta; voltar ao centro ou cancelar o contato não ataca |
| Attack outside deadzone | Mantém a mão selecionada pressionada fora da zona morta; centro/soltura/cancelamento liberam |
| Aim deadzone | 0,15; ajuste entre 0 e 0,60, radial |
| Aim response curve | 1,4; ajuste entre 0,5 e 3,0; valores maiores refinam o início do curso |
| Aim sensitivity / Precise aim | Opções existentes por joystick; cursor preciso passa a usar tempo decorrido, com limite de 50 ms por passo |
| Element opacity | 1,0 para configs antigas; multiplica a opacidade geral e o fade ocioso |
| Layout preset | Padrão, Compacto (80%), Grande (125%), Canhoto (espelha X) |
| Restore built-in controls | Confirmação antes de restaurar ações/rótulos/layout dos IDs padrão; mantém elementos personalizados |
| Save / Load named layout | Escolha manual; confirma substituição de nome existente e permite desfazer a última troca durante a sessão do launcher |
| Game interface scale | **Keep current**, sem override inicial; Automatic=0, Small=1,35, Medium=1,8, Large=2,4, no próximo lançamento |

Presets alteram apenas posição e tamanho dos IDs padrão: preservam ações,
visibilidade, opacidade e todos os elementos personalizados. Layouts nomeados
guardam os elementos completos; ao carregar, mesclam por ID e mantêm outros
elementos que já estavam no editor. Use **Save** para persistir o estado do
launcher e os layouts nomeados. Não há seleção automática por contexto.

A prévia responde imediatamente a posição, tamanho e opacidade. Usa a área
segura e guias aproximadas para vida, hotbar e chat; o editor avisa quando os
controles interceptam essas regiões. Não reposiciona configurações em silêncio.
As guias não são uma captura do HUD em execução: a geometria real depende de
assets, mods e escala. A confirmação final sobre o HUD real é um teste de
aparelho pendente neste ambiente, conforme a seção de limites.

**Keep current** deixa a escala já existente em `starbound.config` sob controle
do jogo. Uma escolha explícita no launcher grava `interfaceScale` na inicialização;
**Automatic** restaura o cálculo automático existente. Não foram mudados seus
limites nem o padrão automático de novas instalações.

## Gestos em janelas

- Toque curto: um clique no destino ao levantar o dedo, sem clique adicional no mundo.
- Lista/crafting/área com `ScrollArea`: arraste para rolar. Há inércia leve,
  amortecimento exponencial e limite de velocidade. Ícones de receitas em listas
  rolam, em vez de iniciar arrasto de inventário.
- Item de inventário: segurar por 450 ms envia um clique direito, além do hover
  usado pelo tooltip. Isso pode separar uma unidade, conforme a UI vanilla.
- Dois dedos próximos no item, antes de arrastar/segurar: Shift+clique para a
  transferência rápida existente. O segundo dedo não assume o cursor.
- Arrastar item: ultrapasse 10 dp de deslocamento para pegar a pilha; solte em
  outro slot para colocá-la. Soltar fora de um slot mantém o item no cursor do
  inventário, sem jogá-lo no mundo e sem clique extra ao fim do arrasto.
- Sliders: toque ou arraste. Botões, abas e setas implementados como widgets
  padrão ganham alvo mínimo de 48 dp equivalentes, sem aumentar seu desenho;
  em sobreposição vence a região visual e depois o alvo mais próximo.
- Tocar fora da janela superior não modal a fecha. Janelas modais bloqueiam
  essa ação. **Voltar** do Android passa pelas ações de Escape/GuiClose/ChatStop
  já configuradas: fecha a janela superior antes de abrir pausa com os bindings padrão.

Os gestos de UI funcionam também quando o overlay está desabilitado. Em gameplay,
**Touchscreen** mantém o cursor sob o dedo e **Touchpad** usa deslocamento relativo;
os bindings diretos de um/dois dedos permanecem os existentes. O limite de tap
passa a ser 10 dp e continua distinto de segurar/arrastar. Não há autoataque
ativado por padrão nem seleção automática de inimigos.

## Cancelamento, teclado e área segura

Cada controle tem um dono. Um segundo contato no mesmo controle é ignorado;
ele precisa de um novo toque após a liberação para assumir esse controle.
Cancelamento SDL, perda de foco, mudança de orientação/tamanho/densidade,
área segura e passagem entre gameplay/janelas/menu liberam ações, macros,
botões, modificadores e rodas. Cancelar não confirma uma roda nem dispara o
ataque de soltura. A abertura de janela no meio do lote SDL também libera as
ações antes dos próximos ataques de toque já enfileirados.

Tooltips não invalidam o arrasto de inventário; abrir/fechar uma janela ou modal
invalida. Cancelar um gesto pendente de UI não solta botões do mouse físico.
Não há novas chamadas de vibração, rumble, haptics ou JNI de feedback tátil.

O campo de texto continua usando SDL Start/StopTextInput. Sua área agora é
convertida do canvas de renderização para a janela SDL incluindo os insets,
para informar o IME na posição correta. As rotinas Android já existentes
`onResume` e `onWindowFocusChanged` reaplicam o modo imersivo. O viewport seguro
existente continua abrangendo o HUD; a prévia também respeita esses insets.
Esses caminhos foram inspecionados e compilados; a posição do teclado, os
insets efetivos e o retorno de background ainda exigem teste no aparelho.
Se **Fullscreen rendering** for habilitado pelo usuário, continua valendo a
opção preexistente de usar toda a tela, inclusive sua área de recorte.

## Compatibilidade verificada

Os caminhos `touch.elements`, ações, modos de pressão e IDs antigos permanecem.
`opacity` ausente recebe 1; opções de mira ausentes recebem os valores da tabela.
O carregador continua mesclando os elementos salvos com os padrões por ID.
Os testes do helper utilizado pelos presets/perfis confirmam preservação de
ID personalizado, ação remapeada e elemento desabilitado, além da inclusão de
novos IDs. A persistência usa merge recursivo e preserva campos desconhecidos
por elemento e nas demais seções do launcher.

Foi feita inspeção do parser e dos seus valores de fallback; não foi executado
um round-trip de configurações reais dentro do jogo. O primeiro cenário manual
abaixo cobre essa integração. Estruturas de saves e código de regras de jogo
não foram editados. As funções de bindings e o adaptador de gamepad mantêm o
conteúdo anterior; os testes independentes de gamepad continuam passando.

## Auditoria de custo

- Padrão: 18 elementos cadastrados, 13 habilitados (11 botões e 2 joysticks),
  D-pad e contador desabilitados. Uma passagem de formas e uma de texto, com
  dois callbacks de blend, mantidas do renderer existente.
- Buffer de rótulos reservado em `setElements` para quatro rótulos por elemento
  (72 no padrão), reutilizado com `clear`. Botão gera no máximo um rótulo;
  D-pad, quatro. Os rótulos referenciam strings estáveis, sem copiá-las por quadro.
- `strokeLines` percorre o `initializer_list`, sem vetor temporário. Tokens e
  legendas de D-pad e ícones são preparados no cache. Dimensões dos rótulos só
  são recalculadas ao mudar fonte/escala/raio ou ao reconstruir o cache.
- Cada rótulo usa nove chamadas `AddText` para contorno + frente, como antes.
  No padrão, só o novo botão L/R requer texto; os demais usam os ícones vetoriais.
- Contadores opcionais usam buffers fixos; texto/medidas são atualizados a
  cada 250 ms. `LogMap::copyValue` evita a cópia de strings. A coleta de RAM
  conserva o cache de 500 ms existente.
- Filas de eventos do adapter e da plataforma são reutilizadas; iteração de
  contatos, repetição de D-pad, macros e expiração de pulsos não copiam coleções
  por quadro. Donos de teclas/ataque são atualizados nas transições.
- O teste mediu **zero alocações em 20.000 ciclos da lógica pura**. A ausência
  de alocações recorrentes na geometria própria do overlay foi auditada por
  código; isso não é medição do allocator/GPU do app inteiro. ImGui pode crescer
  seus buffers no aquecimento, callbacks da UI vanilla/mods podem alocar ao
  tratar eventos e novos contatos/macros/configurações podem alocar nas bordas.
  Não foi medido FPS, energia, latência ou heap no Adreno 610.

## Validação e limites deste ambiente

As saídas reais, comandos e todos os arquivos alterados estão em
[touch-controls-validation.md](touch-controls-validation.md). Os testes puros
compilaram em C++17 com `-Wall -Wextra -Werror`. Os C++ de produção foram
verificados usando o NDK r26d real para arm64/API 28 e compilados individualmente
para Windows launcher e clássico, comparando avisos com `a8cb061`.

**APK completo: não produzido.** NDK, CMake, Ninja e vcpkg foram baixados no
workspace. O configure real de `android-arm64-debug` parou porque o triplet de
ferramentas `x64-windows` exige Visual Studio e não há instância completa
instalada. O SDK/build-tools de empacotamento também não está configurado;
a falha ocorreu antes dessa etapa. Não se alteraram presets, SDK/ABI, assinatura,
compactação, VERSION ou workflow para contornar esse requisito. O YAML de
`.github/workflows/android.yml` foi conferido e permanece idêntico à base.

**Execução no Redmi, screenshots do HUD real, teclado/IME, retomada imersiva e
perfil de GPU/heap: impossíveis de verificar aqui**, sem sessão de aparelho/ADB
e execução do jogo com os assets do usuário. A prévia disponível usa guias de
HUD. A experiência comparável a Terraria é uma meta de usabilidade, não um
resultado de teste afirmado. iOS, Switch e Linux/macOS desktop não foram
compilados nem executados; os caminhos compartilhados mantêm as guardas.
Não foi feito link do cliente Windows, build completo da engine ou CTest de
integração com assets. Widgets personalizados de mods sem os tipos padrão podem
precisar adaptar seus próprios gestos, áreas de toque e rolagem.

## Testes manuais no Redmi Note 13 4G

1. Faça backup das configurações e saves. Inicie com uma configuração antiga:
   confirme posições, botões desabilitados, macros, ações e remapeamentos de
   gamepad. Salve, reabra e confira os dois novos IDs e as chaves personalizadas.
2. Confira o menu principal: controles de gameplay escondidos e um único clique
   por toque. Repita com overlay desabilitado e com mouse físico conectado.
3. Ande devagar, atravesse a zona morta e teste diagonais, escadas e volta ao
   centro. Tente encostar outro dedo no mesmo joystick: não deve roubá-lo.
4. Teste mira direcional e **Precise aim** em 60/90 Hz e com zoom diferente.
   Faça pequenos ajustes, alcance os quatro cantos e confira deadzone/curva.
5. Em **Aim only**, mirar não ataca. Em **Release to attack**, solte fora do
   centro para um ataque e volte ao centro para cancelar. Abra menu ou painel
   do sistema com o dedo ainda pressionado: não pode atacar na retomada.
6. Em **Attack outside deadzone**, segure a mira e caminhe por 30 segundos;
   solte o movimento e continue minerando. Solte a mira e confirme a parada.
   Alterne L/R e compare com os botões L e R separados.
7. Alterne **1 block**, mine e coloque blocos das duas camadas usando as mãos
   existentes. Abra inventário/pausa e confirme a liberação de Shift; repita
   com os bindings de Shift remapeados conforme sua configuração.
8. Abra as duas rodas, arraste para cima/baixo/esquerda/direita e confirme as
   opções. Soltar no centro, perder foco ou abrir modal deve cancelar.
9. Deslize a hotbar para os dois lados, em gestos curtos e longos. Termine fora
   da barra. Não deve haver clique adicional; mouse físico deve continuar normal.
10. No inventário e baú, teste toque simples, segurar 450 ms, dois dedos e
    arrastar pilhas para slots vazios/ocupados. Solte fora da janela e confirme
    que nada foi jogado no chão nem movido em duplicidade.
11. Role crafting/listas começando pelo texto e pelo ícone de receita. Confira
    sentido, inércia curta, limites e interrupção da inércia por novo toque.
12. Teste fechar, abas, setas e sliders pequenos. Toque perto da borda visual;
    a área ampliada deve escolher o vizinho correto, sem mudar sua aparência.
13. Abra janelas sobrepostas e modais. Toque fora de uma janela comum e use
    Voltar várias vezes: feche a superior antes da pausa; modais não fecham
    por toque externo. Confira o comportamento com seus bindings de Escape.
14. Abra chat/campos de texto, digite acentos, envie e cancele pelo Android.
    O teclado não deve cobrir o campo ou deixar Shift/movimento/ataque presos.
15. Com movimento, ataque ou roda ativos, abra notificações/popup, bloqueie a
    tela, volte do background e gire entre as duas orientações de paisagem.
    Retome tocando novamente: nenhuma ação anterior deve permanecer ativa.
16. Aplique cada preset com um botão personalizado presente. Teste opacidade
    individual zero/meia/total, restauração confirmada/cancelada, dois layouts
    nomeados, confirmação de substituição, desfazer e persistência após reiniciar.
17. Confira guias/avisos contra o HUD real com chat e hotbar abertos. Teste as
    escalas pequena/média/grande/automática e notch/barra de gestos nas duas
    orientações, com **Fullscreen rendering** desativado para área segura.
18. Conecte gamepad e mouse, confira gatilhos, R3/remapeamentos, rodas, mira livre,
    inventário e zoom existentes. Jogue por dez minutos com contador desligado,
    depois ligado; observe travamentos/consumo e reporte vídeo/log se houver.
