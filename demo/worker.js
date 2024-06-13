import initKiwi from './kiwi-wasm.js';

let kiwi = null;
let version = null;

async function init() {
    kiwi = await initKiwi();

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

    version = kiwi.VERSION;

    self.postMessage({ type: 'inited', version });
}

function analyze(text) {
    const tokenInfos = kiwi.analyze(text);
    
    // This needs to be converted because only basic types can be transferred to the main context
    const result = [];

    for (let i = 0; i < tokenInfos.size(); i++) {
        const tokenInfo = tokenInfos.get(i);

        result.push({
            position: tokenInfo.position,
            length: tokenInfo.length,
            str: tokenInfo.str,
            tag: tokenInfo.tagToString(),
        });
    }


    self.postMessage({ type: 'analyzed', result, text });
}

self.onmessage = (event) => {
    const { type } = event.data;

    switch (type) {
        case 'init':
            init();
            break;
        case 'analyze':
            analyze(event.data.text);
            break;
        default:
            console.error(`Unknown message type: ${type}`);
    }
};
