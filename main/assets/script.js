// ======================= Websocket Code =========================

// Get the ESP32's IP address (it's always 192.168.4.1 for the access point)
const ws = new WebSocket('ws://192.168.4.1/ws');

ws.onopen = function() {
    console.log('WebSocket connected!');
};

ws.onmessage = function(event) {
    console.log('Received from ESP32:', event.data);
    
    try {
        const data = JSON.parse(event.data);
        
        if (data.type === 'result') {
            console.log('Guess result:', data.word, data.result);
            
            // Convert server format (0,1,2) to CSS classes (absent,present,correct)
            const evalRes = data.result.map(val => {
                if (val === 2) return 'correct';
                if (val === 1) return 'present';
                return 'absent';
            });
            
            // Fill current row with the guess
            for (let c = 0; c < 5; c++) {
                boardState[currentRow][c] = data.word[c];
            }
            lockedRows[currentRow] = true;
            
            renderBoard();
            applyEvaluationToRow(currentRow, evalRes);
            
            // Check for win
            if (evalRes.every(x => x === 'correct')) {
                statusEl.innerText = `Nice! You won!`;
                gameOver = true;
                restartBtn.style.display = "inline-block";
            } else {
                advanceToNextRow();
            }
        }
    } catch (e) {
        console.log('Plain message:', event.data);
    }
};

// Function to send a guess
function sendGuess(word) {
    const message = {
        type: 'guess',
        word: word.toUpperCase()
    };
    ws.send(JSON.stringify(message));
}




/* ---------- Configuration ---------- */
const SECRET = "CRANE" // Word must be 5 letters
const MAX_ROWS = 5;

/* ---------- Game State ---------- */
let playerName = "";
let playersJoined = 1;     // simulate first player has joined
let currentRow = 0;
let boardState = Array.from({length: MAX_ROWS}, () => Array(5).fill(""));
let lockedRows = Array(MAX_ROWS).fill(false);
let gameOver = false;

/* ---------- UI Elements ---------- */
const boardEl = document.getElementById("board");
const inputEl = document.getElementById("guessInput");
const statusEl = document.getElementById("gameStatus");
const lobbyStatusEl = document.getElementById("lobbyStatus");
const restartBtn = document.getElementById("restartBtn");

/* ---------- Initialization ---------- */
function join(){
  playerName = document.getElementById("name").value.trim();
  if(!playerName) return alert("Please enter a name.");
  document.getElementById("namePage").style.display = "none";
  document.getElementById("lobbyPage").style.display = "block";
  lobbyStatusEl.innerText = "Waiting for players... (1/2)";

  // Simulate second player joining shortly after
  setTimeout(() => {
    playersJoined = 2;
    lobbyStatusEl.innerText = "2/2 Players joined!";
    setTimeout(startGame, 800);
  }, 1200);
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
      
      // Preserve colors for locked rows
      if (lockedRows[r]) {
        const evalRes = evaluateGuess(boardState[r].join(''), SECRET);
        tile.classList.add(evalRes[c]);
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
  // update board visual for current row
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

/* ---------- Guess Checking (Wordle rules, handles duplicates) ---------- */
function evaluateGuess(guess, secret) {
  const result = Array(5).fill('absent');
  guess = guess.toUpperCase();
  secret = secret.toUpperCase();

  // mark correct first
  const secretChars = secret.split('');
  const guessChars = guess.split('');
  const remaining = {};

  for (let i = 0; i < 5; i++) {
    if (guessChars[i] === secretChars[i]) {
      result[i] = 'correct';
      secretChars[i] = null;  // consumed
      guessChars[i] = null;
    }
  }

  // build counts of remaining secret letters
  for (let i = 0; i < 5; i++) {
    if (secretChars[i]) remaining[secretChars[i]] = (remaining[secretChars[i]]||0) + 1;
  }

  // mark present
  for (let i = 0; i < 5; i++) {
    if (!guessChars[i]) continue; // already correct
    const ch = guessChars[i];
    if (remaining[ch]) {
      result[i] = 'present';
      remaining[ch]--;
    } else {
      result[i] = 'absent';
    }
  }

  return result; // array of 'correct'|'present'|'absent'
}

/* ---------- Submit Guess ---------- */
function submitGuess(){
  if (gameOver) return;
  const guess = inputEl.value.trim().toUpperCase();
  if (guess.length !== 5) {
    statusEl.innerText = "Guess must be 5 letters.";
    return;
  }

  if (lockedRows[currentRow]) {
    statusEl.innerText = "Row already submitted.";
    return;
  }

  // Send guess to ESP32
  sendGuess(guess);
  
  statusEl.innerText = "Guess sent, waiting for result...";
  inputEl.value = "";
}

/* ---------- Apply coloring to tiles for a row ---------- */
function applyEvaluationToRow(rowIndex, evalRes) {
  const rowEl = document.querySelector(`.row[data-row='${rowIndex}']`);
  if (!rowEl) return;
  for (let c = 0; c < 5; c++) {
    const tile = rowEl.querySelector(`.tile[data-col='${c}']`);
    if (!tile) continue;
    tile.classList.remove('correct','present','absent');
    tile.classList.add(evalRes[c]);
  }
}

/* ---------- Advance Row ---------- */
function advanceToNextRow() {
  currentRow++;
  if (currentRow >= MAX_ROWS) {
    // game over: out of rows
    statusEl.innerText = `Out of guesses — the word was "${SECRET}".`;
    gameOver = true;
    revealSecret();
    restartBtn.style.display = "inline-block";
    return;
  }
  inputEl.value = "";
  highlightCurrentRow();
  inputEl.focus();
}

/* ---------- Endgame helpers ---------- */
function revealSecret() {
  // Show secret on last row if not guessed (visual aid - optional)
  // color each letter according to correctness (all correct when revealed)
  const finalEval = evaluateGuess(SECRET, SECRET);
  applyEvaluationToRow(Math.min(currentRow, MAX_ROWS-1), finalEval);
}

/* ---------- Restart ---------- */
function restart() {
  resetBoardState();
  renderBoard();
  statusEl.innerText = "";
  inputEl.value = "";
  inputEl.focus();
  restartBtn.style.display = "none";
}

/* ---------- Initial render for page load (keeps things tidy) ---------- */
resetBoardState();
renderBoard();



