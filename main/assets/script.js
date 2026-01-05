// ======================= Websocket Code =========================

// Get the ESP32's IP address (it's always 192.168.4.1 for the access point)
const ws = new WebSocket('ws://192.168.4.1/ws');

let wsReady = false;

ws.onopen = function() {
    console.log('WebSocket connected!');
    wsReady = true;
    // Enable the join button
    const joinBtn = document.getElementById("joinBtn");
    if (joinBtn) {
        joinBtn.disabled = false;
        joinBtn.innerText = "Join Game";
    }
};

ws.onerror = function(error) {
    console.log('WebSocket error:', error);
    const joinBtn = document.getElementById("joinBtn");
    if (joinBtn) {
        joinBtn.innerText = "Connection Failed - Refresh Page";
    }
};

ws.onclose = function(event) {
    console.log('WebSocket closed');
    wsReady = false;
};

ws.onmessage = function(event) {
    try {
        // CHANGE 1 & 2: Added JSON.parse and proper try block wrapping
        const data = JSON.parse(event.data);

        if (data.type === 'welcome') {
            playerIndex = data.player_index;
            console.log('I am player index:', playerIndex);
        }
        else if (data.type === 'lobby_update') {
            // ADD THIS HANDLER
            lobbyStatusEl.innerText = `Waiting for players... (${data.player_count}/2)`;
        }
        else if (data.type === 'game_starting') {
            lobbyStatusEl.innerText = "2/2 Players joined!";
            setTimeout(startGame, 800);
        }
        else if (data.type === 'round_start') {
          console.log('Round', data.round, 'starting!');
          roundNumber = data.round;
          roundActive = true;
          gameOver = false;
          waitingForOpponent = false;
          opponentWaiting = false;

          // Check if we got a hint for THIS round
          if (data.hint_player !== undefined && data.hint_player === playerIndex) {
              currentHint = {
                  type: data.hint_type,
                  letter: data.hint_letter,
                  position: data.hint_position
              };
              console.log('Received hint:', currentHint);
          } else {
              currentHint = null;
          }

          resetBoardState();
          renderBoard();
          updateScoreDisplay();

          statusEl.innerText = `Round ${roundNumber} - Guess 1/5${getHintText()}`;
          inputEl.focus();
        }
        else if (data.type === 'result') {
            // CHANGE 3 & 4: Consolidated duplicate result logic and fixed indentation
            console.log('Player', data.player, 'guessed:', data.word, 'Result:', data.result);

            if (data.player === playerIndex) {
                // Our guess - update our board
                const evalRes = data.result.map(val => {
                    if (val === 2) return 'correct';
                    if (val === 1) return 'present';
                    return 'absent';
                });

                for (let c = 0; c < 5; c++) {
                    boardState[currentRow][c] = data.word[c];
                }
                lockedRows[currentRow] = true;

                renderBoard();
                applyEvaluationToRow(currentRow, evalRes);

                if (data.is_correct) {
                    statusEl.innerText = `You got it! Waiting for round to end...`;
                    gameOver = true;
                    // stopTimer();
                } else {
                    waitingForOpponent = true;
                    statusEl.innerText = "Waiting for opponent to guess...";
                    // stopTimer();
                }
            } else {
                // Opponent's guess
                if (data.is_correct) {
                    // statusEl.innerText = `Opponent guessed it! You have ${timeRemaining}s left`;
                    statusEl.innerText = `Waiting for opponent to guess...${getHintText()}`;
                }
            }
        }
        else if (data.type === 'round_end') {
          console.log('Round ended. Winner:', data.winner);
          roundActive = false;
          gameOver = true;

          myScore = data.player1_score;
          opponentScore = data.player2_score;
          updateScoreDisplay();

          let resultMsg = "";
          if (data.winner === -1) {
              resultMsg = "Tie!";
          } else if (data.winner === playerIndex) {
              resultMsg = "You won this round!";
          } else {
              resultMsg = "Opponent won this round!";
          }

          resultMsg += ` | Score: You ${myScore} - ${opponentScore} Opponent`;
          resultMsg += ` | Word was: ${data.target_word}`;
          
          statusEl.innerText = resultMsg;

          restartBtn.innerText = "Next Round";
          restartBtn.style.display = "inline-block";
      }
        else if (data.type === 'opponent_submitted') {
            console.log('Opponent submitted');
            opponentWaiting = true;
            if (!waitingForOpponent && !gameOver) {
                // statusEl.innerText = `Opponent submitted! Your turn - Time: ${timeRemaining}s`;
                statusEl.innerText = `Opponent submitted! Your turn${getHintText()}`;
            }
        }
        else if (data.type === 'both_guessed') {
            console.log('Both players guessed, advancing');
            waitingForOpponent = false;
            opponentWaiting = false;

            if (!gameOver) {
                currentRow++;
                if (currentRow >= MAX_ROWS) {
                    statusEl.innerText = "Out of guesses! Waiting for round to end...";
                    gameOver = true;
                } else {
                    // startTimer();
                    // statusEl.innerText = `Guess ${currentRow + 1}/5 - Time: 45s`;
                    updateScoreDisplay();
                    statusEl.innerText = `Guess ${currentRow + 1}/5${getHintText()}`;
                    inputEl.focus();
                }
            }
        }
        // else if (data.type === 'timeout') {
        //     console.log('Timed out on this guess');
        //     stopTimer();
        //     statusEl.innerText = "Time's up! Skipping this guess...";
        //     waitingForOpponent = true;
        // }

    } catch (e) {
        console.log('Plain message:', event.data);
        if (event.data.includes("You are Player")) {
            if (event.data.includes("Player 1")) {
                playerIndex = 0;
            } else if (event.data.includes("Player 2")) {
                playerIndex = 1;
            }
            console.log('I am player index:', playerIndex);
        }
    }
};

function sendGuess(word) {
    const message = {
        type: 'guess',
        word: word.toUpperCase()
    };
    ws.send(JSON.stringify(message));
}

/* ---------- Configuration ---------- */
const MAX_ROWS = 5;

/* ---------- Game State ---------- */
let playerName = "";
let playerIndex = -1;
let currentRow = 0;
let boardState = Array.from({length: MAX_ROWS}, () => Array(5).fill(""));
let boardColors = Array.from({length: MAX_ROWS}, () => Array(5).fill(""));  // ADD THIS
let lockedRows = Array(MAX_ROWS).fill(false);
let gameOver = false;
let roundActive = false;
// let timeRemaining = 45;
// let timerInterval = null;
let myScore = 0;
let opponentScore = 0;
let roundNumber = 0;
let waitingForOpponent = false;
let opponentWaiting = false;
let currentHint = null;



/* ---------- UI Elements ---------- */
const boardEl = document.getElementById("board");
const inputEl = document.getElementById("guessInput");
const statusEl = document.getElementById("gameStatus");
const lobbyStatusEl = document.getElementById("lobbyStatus");
const restartBtn = document.getElementById("restartBtn");

/* ------------- Timing --------------- */
// function updateTimer() {
//     if (timeRemaining > 0) {
//         timeRemaining--;
//         statusEl.innerText = `Time: ${timeRemaining}s`;
//         if (timeRemaining === 0) {
//             statusEl.innerText = "Time's up! Waiting for results...";
//             gameOver = true;
//         }
//     }
// }

// function startTimer() {
//     if (timerInterval) clearInterval(timerInterval);
//     timeRemaining = 45;
//     statusEl.innerText = `Guess ${currentRow + 1}/5 - Time: 45s`;
//     timerInterval = setInterval(updateTimer, 1000);
// }

// function stopTimer() {
//     if (timerInterval) {
//         clearInterval(timerInterval);
//         timerInterval = null;
//     }
// }

/* ---------- Initialization ---------- */

function sendJoinMessage() {
    const message = { type: 'join', name: playerName };
    ws.send(JSON.stringify(message));
    
    document.getElementById("namePage").style.display = "none";
    document.getElementById("lobbyPage").style.display = "block";
    lobbyStatusEl.innerText = "Waiting for players... (1/2)";
}

function join(){
    playerName = document.getElementById("name").value.trim();
    if(!playerName) return alert("Please enter a name.");
    
    if (!wsReady) {
        alert("Please wait for connection...");
        return;
    }
    
    const message = { type: 'join', name: playerName };
    ws.send(JSON.stringify(message));
    
    document.getElementById("namePage").style.display = "none";
    document.getElementById("lobbyPage").style.display = "block";
    lobbyStatusEl.innerText = "Waiting for players... (1/2)";
}

function startGame(){
    document.getElementById("lobbyPage").style.display = "none";
    document.getElementById("gamePage").style.display = "block";
    resetBoardState();
    renderBoard();
    inputEl.focus();
}

/* ---------- Board Rendering ---------- */
function resetBoardState() {
    currentRow = 0;
    boardState = Array.from({length: MAX_ROWS}, () => Array(5).fill(""));
    boardColors = Array.from({length: MAX_ROWS}, () => Array(5).fill(""));  // ADD THIS
    lockedRows = Array(MAX_ROWS).fill(false);
    gameOver = false;
    statusEl.innerText = "";
    restartBtn.style.display = "none";
}

function renderBoard() {
    boardEl.innerHTML = "";
    for (let r = 0; r < MAX_ROWS; r++) {
        const row = document.createElement("div");
        row.className = "row";
        row.dataset.row = r;
        for (let c = 0; c < 5; c++) {
            const tile = document.createElement("div");
            tile.className = "tile";
            tile.dataset.row = r;
            tile.dataset.col = c;
            tile.innerText = boardState[r][c] || "";
            
            // Reapply saved colors if they exist
            if (boardColors[r][c]) {
                tile.classList.add(boardColors[r][c]);
            }
            
            row.appendChild(tile);
        }
        boardEl.appendChild(row);
    }
    highlightCurrentRow();
}

function highlightCurrentRow() {
    document.querySelectorAll(".row").forEach(row => {
        if (parseInt(row.dataset.row,10) === currentRow && !gameOver) {
            row.style.opacity = "1";
        } else {
            row.style.opacity = "0.8";
        }
    });
}

/* ---------- Input Helpers ---------- */
inputEl.addEventListener("input", (e) => {
    if (gameOver) return;
    let val = e.target.value.toUpperCase().replace(/[^A-Z]/g,'');
    if (val.length > 5) val = val.slice(0,5);
    e.target.value = val;
    for (let c = 0; c < 5; c++) {
        boardState[currentRow][c] = val[c] || "";
    }
    renderBoard();
});

function deleteLetter() {
    if (gameOver) return;
    inputEl.value = inputEl.value.slice(0,-1);
    inputEl.dispatchEvent(new Event('input'));
}

/* ---------- Guess Checking ---------- */
function evaluateGuess(guess, secret) {
    const result = Array(5).fill('absent');
    guess = guess.toUpperCase();
    secret = secret.toUpperCase();
    const secretChars = secret.split('');
    const guessChars = guess.split('');
    const remaining = {};

    for (let i = 0; i < 5; i++) {
        if (guessChars[i] === secretChars[i]) {
            result[i] = 'correct';
            secretChars[i] = null;
            guessChars[i] = null;
        }
    }
    for (let i = 0; i < 5; i++) {
        if (secretChars[i]) remaining[secretChars[i]] = (remaining[secretChars[i]]||0) + 1;
    }
    for (let i = 0; i < 5; i++) {
        if (!guessChars[i]) continue;
        const ch = guessChars[i];
        if (remaining[ch]) {
            result[i] = 'present';
            remaining[ch]--;
        } else {
            result[i] = 'absent';
        }
    }
    return result;
}

function submitGuess(){
    if (gameOver || waitingForOpponent) {
        statusEl.innerText = (waitingForOpponent ? "Already submitted, wait for opponent" : "Round over") + getHintText();
        return;
    }
    const guess = inputEl.value.trim().toUpperCase();
    if (guess.length !== 5) {
        statusEl.innerText = `Guess must be 5 letters.${getHintText()}`;
        return;
    }
    sendGuess(guess);
    statusEl.innerText = `Submitting guess...${getHintText()}`;
    inputEl.value = "";
}

function applyEvaluationToRow(rowIndex, evalRes) {
    // Save the colors to our state
    for (let c = 0; c < 5; c++) {
        boardColors[rowIndex][c] = evalRes[c];  // ADD THIS
    }
    
    // Apply to DOM
    const rowEl = document.querySelector(`.row[data-row='${rowIndex}']`);
    if (!rowEl) return;
    for (let c = 0; c < 5; c++) {
        const tile = rowEl.querySelector(`.tile[data-col='${c}']`);
        if (!tile) continue;
        tile.classList.remove('correct','present','absent');
        tile.classList.add(evalRes[c]);
    }
}

function restart() {
    const message = { type: 'next_round' };
    ws.send(JSON.stringify(message));
    restartBtn.style.display = "none";
}

function getHintText() {
    if (!currentHint) return "";
    
    if (currentHint.type === "green") {
        return ` | Hint: Position ${currentHint.position + 1} is "${currentHint.letter}"`;
    } else {
        return ` | Hint: Word contains "${currentHint.letter}"`;
    }
}

function updateScoreDisplay() {
    // Find or create score display
    let scoreDisplay = document.getElementById('scoreDisplay');
    if (!scoreDisplay) {
        scoreDisplay = document.createElement('div');
        scoreDisplay.id = 'scoreDisplay';
        scoreDisplay.style.cssText = `
            background: #f0f0f0;
            padding: 10px 15px;
            border-radius: 5px;
            margin-bottom: 15px;
            font-weight: bold;
            color: #333;
        `;
        // Insert before the board
        const board = document.getElementById('board');
        board.parentNode.insertBefore(scoreDisplay, board);
    }
    
    scoreDisplay.innerText = `Score: You ${myScore} - ${opponentScore} Opponent`;
}

// Initial render
resetBoardState();
renderBoard();