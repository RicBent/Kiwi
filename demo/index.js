import init_kiwi from './kiwi-wasm.js';

const elVersion = document.getElementById('version');
const elInput = document.getElementById('input');
const elResult = document.getElementById('result');

const kiwiPromise = init_kiwi();

globalThis.kiwi = null;
globalThis.kiwiPromise = kiwiPromise;

kiwiPromise.then(async (kiwi) => {
    globalThis.kiwi = kiwi;
    await init(kiwi);
});

async function init(kiwi) {
    const requiredFiles = [
        'combiningRule.txt',
        'default.dict',
        'extract.mdl',
        'multi.dict',
        'sj.knlm',
        'sj.morph',
        'skipbigram.mdl',
        'typo.dict',
    ];

    const files = await Promise.all(
        requiredFiles.map(async (file) => {
            const response = await fetch(`./model/${file}`);

            if (!response.ok) {
                throw new Error(`Failed to fetch ${file}`);
            }

            const data = await response.arrayBuffer();

            return {
                name: file,
                data,
            };
        })
    );

    kiwi.FS.mkdir('model');
    for (const file of files) {
        kiwi.FS.writeFile(`model/${file.name}`, new Uint8Array(file.data));
    }
    kiwi.init('model');

    elVersion.innerText = 'v' + kiwi.VERSION;

    elInput.hidden = false;
    analyze();
    input.addEventListener('input', analyze);
}

function analyze() {
    const text = elInput.value;
    const result = kiwi.analyze(text);

    while (elResult.rows.length > 1) {
        elResult.deleteRow(1);
    }

    for (let i = 0; i < result.size(); i++) {
        const tokenInfo = result.get(i);

        const surface = text.substring(
            tokenInfo.position,
            tokenInfo.position + tokenInfo.length
        );

        const row = elResult.insertRow();
        row.insertCell().innerText = tokenInfo.position;
        row.insertCell().innerText = tokenInfo.length;
        row.insertCell().innerText = surface;
        row.insertCell().innerText = tokenInfo.str;
        row.insertCell().innerText = tokenInfo.tagToString();
    }

    elResult.hidden = result.size() === 0;
}
