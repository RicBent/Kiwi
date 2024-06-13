const elVersion = document.getElementById('version');
const elInput = document.getElementById('input');
const elResult = document.getElementById('result');

const worker = new Worker('./worker.js', { type: 'module' });
worker.onmessage = (event) => {
    const data = event.data ?? {};

    switch (data.type) {
        case 'inited':
            inited(data.version);
            break;
        case 'analyzed':
            analyzed(data.result, data.text);
            break;
        default:
            console.error('Unknown worker message', data);
            break;
    }
};
worker.postMessage({ type: 'init' });

function inited(version) {
    elVersion.innerText = 'v' + version;

    elInput.hidden = false;
    input.addEventListener('input', analyze);
    analyze();
}

function analyze(text = elInput.value) {
    worker.postMessage({ type: 'analyze', text });
}

function analyzed(tokenInfos, text) {
    while (elResult.rows.length > 1) {
        elResult.deleteRow(1);
    }

    for (const tokenInfo of tokenInfos) {
        const surface = text.substring(
            tokenInfo.position,
            tokenInfo.position + tokenInfo.length
        );

        const row = elResult.insertRow();
        row.insertCell().innerText = tokenInfo.position;
        row.insertCell().innerText = tokenInfo.length;
        row.insertCell().innerText = surface;
        row.insertCell().innerText = tokenInfo.str;
        row.insertCell().innerText = tokenInfo.tag;
    }

    elResult.hidden = tokenInfos.length === 0;
}
