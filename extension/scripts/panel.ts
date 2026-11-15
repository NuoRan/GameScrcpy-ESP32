type BoardState = "ready" | "busy" | "problem" | "offline";

type Board = {
    id: string;
    name: string;
    where: string;
    via: "serial" | "wifi" | "ble";
    wifi: boolean;
    bluetooth: boolean;
    state: BoardState;
    stateText: string;
    summary: string;
    connected: boolean;
    legacy: boolean;
    inSession: boolean;
    flashing: boolean;
    held: boolean;
    yieldedTo: string;
    chip: string | null;
    firmware: string | null;
    boardName: string | null;
    hasWifi: boolean;
    hasBluetoothLink: boolean;
    parts: string;
    phone: string;
    wifiText: string;
    wifiConfigured: boolean;
    address: string | null;
    roundTripMs: number | null;
    slowTripMs: number | null;
    slowBluetooth: boolean;
    slowText: string;
    signal: number | null;
    signalText: string;
    problem: string;
    bundled: string | null;
    upgradable: boolean;
};

type NearbyBoard = {
    address: string;
    id: string;
    name: string;
    chip: string;
    signal: number;
    signalText: string;
    keyed: boolean;
    busy: boolean;
    known: boolean;
    paired: boolean;
    connected: boolean;
};

type PairedBoard = { id: string; name: string; address: string };

type Radio = {
    state: "missing" | "off" | "ready" | "unknown";
    stateText: string;
    scanning: boolean;
    boards: NearbyBoard[];
    paired: PairedBoard[];
};

type Work = { device: string; running: boolean; fraction: number; text: string };

type SerialPort = { name: string; title: string; kind: string; likely: boolean };

type Firmware = { version: string; targets: { target: string; chip: string }[] } | null;

type Fields = { [key: string]: StoreValue };

type FlashState = {
    device: string;
    port: string;
    running: boolean;
    fraction: number;
    stage: string;
    text: string;
    failed: boolean;
};

type Note = { text: string; failed: boolean };

type Esp32 = {
    boards(): Board[];
    ports(): SerialPort[];
    firmware(): Firmware;
    problem(): string;
    language(): string;
    tap(device: string): boolean;
    disconnect(device: string): boolean;
    rescan(): boolean;
    bluetooth(): Radio;
    flash(args: { device: string }): ExtensionJob<StoreValue>;
    wifi(args: { device: string; ssid: string; password: string }): ExtensionJob<StoreValue>;
    rename(args: { device: string; name: string }): ExtensionJob<StoreValue>;
    diagnose(args: { device: string }): ExtensionJob<StoreValue>;
    scan(args: Record<string, never>): ExtensionJob<StoreValue>;
    pair(args: { device: string }): ExtensionJob<StoreValue>;
    unpair(args: { device: string }): ExtensionJob<StoreValue>;
};

const esp32 = (): Esp32 => ext.community.esp32 as Esp32;

const english = {
    intro: "Plug the board's serial port into this computer, or set up its Wi-Fi once. Each phone picks the board in its own settings of this extension.",
    boards: "Boards",
    noBoards: "No ESP32 touch box was found yet.",
    rescan: "Look again",
    firmware: "Firmware",
    bundled: "Comes with the extension",
    noFirmware: "This copy of the extension has no firmware with it.",
    port: "Serial port",
    noPorts: "No serial port found. Plug the board's serial port into this computer.",
    flash: "Flash firmware",
    upgrade: "Upgrade firmware",
    bootHint: "If it says the board did not enter download mode: hold BOOT while plugging in the cable, then flash again.",
    flashSerialOnly: "Firmware is flashed over the serial port: connect the board with its cable first.",
    flashSteps: ["Prepare", "Download mode", "Write", "Check", "Restart"],
    chip: "Chip",
    version: "Firmware",
    upToDate: "%1 · up to date",
    available: "%1 · %2 available",
    oldFirmware: "Old firmware",
    where: "Connected through",
    phone: "Phone",
    wifi: "Wi-Fi",
    parts: "Parts",
    roundTrip: "Round trip",
    milliseconds: "%1 ms",
    tryIt: "Try it",
    tapped: "Tapped the middle of the phone's screen.",
    notTapped: "The tap did not reach the board.",
    disconnect: "Disconnect",
    gone: "This board is not connected any more. Plug it in again, or look for boards again on the Extensions page.",
    wifiGroup: "Wi-Fi",
    wifiIntro: "The board joins this network and can then be reached without the cable. Only this computer gets the key that lets it talk to the board.",
    wifiSerialOnly: "Wi-Fi is set over the serial port: connect the board with its cable first.",
    wifiMissing: "This board has no Wi-Fi.",
    ssid: "Network name",
    password: "Password",
    saveWifi: "Save Wi-Fi",
    forgetWifi: "Forget Wi-Fi",
    wifiSent: "Sent. The board joins the network now.",
    wifiForgotten: "The board forgot its Wi-Fi.",
    nameGroup: "Name",
    name: "Board name",
    rename: "Rename",
    renamed: "Renamed.",
    diagnosisGroup: "Diagnosis",
    diagnose: "Diagnose",
    healthy: "The board answers and works.",
    bluetoothGroup: "Bluetooth",
    bluetoothIntro:
        "With firmware 1.1 or later, a classic ESP32, C3, C6 or S3 board can reach this computer over Bluetooth instead of the cable. Pair it once from its page while it is plugged in by USB, or below when this computer already has its key.",
    lookForBoards: "Look for boards",
    noneNearby: "No board was found nearby. Keep the board on and near this computer, then look again.",
    needsKey: "A board marked No key has not met this computer yet: plug it in by USB and pair it from its page.",
    pair: "Pair",
    forgetPairing: "Forget pairing",
    paired: "Paired",
    notPaired: "Not paired",
    noKey: "No key",
    inUse: "Connected",
    busyElsewhere: "In use",
    notNearby: "Not nearby",
    pairedDone: "Paired. Choose Bluetooth as the ESP32 connection in the phone's settings of this extension.",
    unpaired: "The pairing is forgotten.",
    viaBluetooth: "Bluetooth",
    slowTrip: "Slowest 1 in 100",
    pairIntro: "Pair this board with this computer over Bluetooth so the phone can use it without the cable. It gets its key over the cable first.",
    pairBoard: "Pair over Bluetooth",
    bluetoothLinked: "This board reaches this computer over Bluetooth.",
    noBluetoothLink: "This firmware cannot reach a computer over Bluetooth. Flash firmware 1.1 or later; the ESP32-S2 has no Bluetooth.",
    pairSerialOnly: "Pairing starts over the serial port: connect the board with its cable first, or pair it from the boards found over Bluetooth on the Extensions page.",
    help: "Wiring, flashing and the messages are explained in the help article ESP32 touch box.",
};

const chinese: typeof english = {
    intro: "把开发板的串口接到这台电脑，或者先给它设置一次 Wi-Fi。每台手机在这个扩展的手机设置里选用哪块开发板。",
    boards: "开发板",
    noBoards: "还没有找到 ESP32 触控盒。",
    rescan: "重新查找",
    firmware: "固件",
    bundled: "扩展附带",
    noFirmware: "这份扩展没有附带固件。",
    port: "串口",
    noPorts: "没有找到串口。把开发板的串口接到这台电脑。",
    flash: "刷写固件",
    upgrade: "升级固件",
    bootHint: "如果提示开发板没有进入下载模式：按住 BOOT 再插上数据线，然后重新刷写。",
    flashSerialOnly: "固件要经串口刷写：请先用数据线连上开发板。",
    flashSteps: ["准备", "下载模式", "写入", "校验", "重启"],
    chip: "芯片",
    version: "固件",
    upToDate: "%1 · 已是最新",
    available: "%1 · 有新版本 %2",
    oldFirmware: "旧固件",
    where: "连接方式",
    phone: "手机",
    wifi: "Wi-Fi",
    parts: "各部分",
    roundTrip: "往返延迟",
    milliseconds: "%1 ms",
    tryIt: "试一下",
    tapped: "已在手机屏幕中央点了一下。",
    notTapped: "这一下没有发到开发板。",
    disconnect: "断开",
    gone: "这块开发板已经断开。重新插上，或者在扩展页重新查找开发板。",
    wifiGroup: "Wi-Fi",
    wifiIntro: "开发板会加入这个网络，之后不接数据线也能连接。只有这台电脑拿到与开发板通信的密钥。",
    wifiSerialOnly: "Wi-Fi 要经串口设置：请先用数据线连上开发板。",
    wifiMissing: "这块开发板没有 Wi-Fi。",
    ssid: "网络名称",
    password: "密码",
    saveWifi: "保存 Wi-Fi",
    forgetWifi: "忘记 Wi-Fi",
    wifiSent: "已发送，开发板正在加入网络。",
    wifiForgotten: "开发板已忘记 Wi-Fi。",
    nameGroup: "名称",
    name: "开发板名称",
    rename: "重命名",
    renamed: "已重命名。",
    diagnosisGroup: "诊断",
    diagnose: "诊断",
    healthy: "开发板有应答，工作正常。",
    bluetoothGroup: "蓝牙",
    bluetoothIntro:
        "装了 1.1 或更新固件的经典 ESP32、C3、C6、S3 开发板可以经蓝牙连这台电脑，不用数据线。开发板经 USB 接着时在它的页面配对一次；这台电脑已有它的密钥时也可以在下面配对。",
    lookForBoards: "查找开发板",
    noneNearby: "附近没有找到开发板。让开发板保持通电、靠近这台电脑，再找一次。",
    needsKey: "标着“没有密钥”的开发板还没见过这台电脑：先经 USB 接上，在它的页面配对。",
    pair: "配对",
    forgetPairing: "忘记配对",
    paired: "已配对",
    notPaired: "未配对",
    noKey: "没有密钥",
    inUse: "已连接",
    busyElsewhere: "使用中",
    notNearby: "不在附近",
    pairedDone: "已配对。在这台手机的扩展设置里把 ESP32 连接方式选为蓝牙。",
    unpaired: "已忘记配对。",
    viaBluetooth: "蓝牙",
    slowTrip: "最慢的百分之一",
    pairIntro: "让这块开发板经蓝牙与这台电脑配对，手机就能不接数据线使用它。开发板会先经数据线拿到密钥。",
    pairBoard: "经蓝牙配对",
    bluetoothLinked: "这块开发板经蓝牙连着这台电脑。",
    noBluetoothLink: "这个固件还不能经蓝牙连电脑。请刷入 1.1 或更新的固件；ESP32-S2 没有蓝牙。",
    pairSerialOnly: "配对要从串口开始：请先用数据线连上开发板，或者在扩展页从经蓝牙找到的开发板里配对。",
    help: "接线、刷写与各种提示的意思见帮助文章“ESP32 触控盒”。",
};

const say = safely(() => esp32().language(), "en") === "zh_CN" ? chinese : english;

const main = ui.panel("main");
const sheet = ui.panel("board");

let mainShown = false;
let shownBoard = "";
let chosenPort = "";
let flash: FlashState = { device: "", port: "", running: false, fraction: 0, stage: "", text: "", failed: false };
let ssid = "";
let password = "";
let boardName = "";
let tapNote: Note | null = null;
let wifiNote: Note | null = null;
let renameNote: Note | null = null;
let diagnosis: { device: string; ok: boolean; text: string; rows: { key: string; value: string }[] } | null = null;
const idle: Work = { device: "", running: false, fraction: 0, text: "" };
let scan: Work = idle;
let scanned = false;
let pairing: Work = idle;
let bluetoothNote: Note | null = null;
let pairNote: Note | null = null;

function safely<T>(work: () => T, fallback: T): T {
    try {
        return work();
    } catch (error) {
        log.warn("ESP32 touch box:", messageOf(error));
        return fallback;
    }
}

function messageOf(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

function filled(pattern: string, ...values: string[]): string {
    let text = pattern;
    values.forEach((value, index) => {
        text = text.replace("%" + (index + 1), value);
    });
    return text;
}

function fieldsOf(value: StoreValue): Fields {
    return value !== null && typeof value === "object" && !Array.isArray(value) ? value : {};
}

function textIn(fields: Fields, key: string): string {
    const found = fields[key];
    return typeof found === "string" ? found : "";
}

function numberIn(fields: Fields, key: string): number {
    const found = fields[key];
    return typeof found === "number" ? found : 0;
}

function boardsNow(): Board[] {
    const listed = safely(() => esp32().boards(), []);
    return Array.isArray(listed) ? listed : [];
}

function portsNow(): SerialPort[] {
    const listed = safely(() => esp32().ports(), []);
    return Array.isArray(listed) ? listed : [];
}

function radioNow(): Radio {
    const found = safely<Radio | null>(() => esp32().bluetooth(), null);
    if (found === null || typeof found !== "object" || !Array.isArray(found.boards) || !Array.isArray(found.paired)) {
        return { state: "unknown", stateText: "", scanning: false, boards: [], paired: [] };
    }
    return found;
}

function workNodes(work: Work): UiNode[] {
    if (!work.running) {
        return [];
    }
    return [{ type: "progress", title: work.text, value: work.fraction >= 0 ? work.fraction : null }];
}

function stepOf(stage: string): number {
    switch (stage) {
        case "preparing":
            return 0;
        case "connecting":
        case "detecting":
            return 1;
        case "erasing":
        case "writing":
            return 2;
        case "verifying":
            return 3;
        case "restarting":
            return 4;
        case "done":
            return 5;
        default:
            return 0;
    }
}

function noteNode(note: Note | null): UiNode[] {
    if (note === null || note.text === "") {
        return [];
    }
    return [{ type: "text", text: note.text, style: "caption", tone: note.failed ? "bad" : "good" }];
}

function flashNodes(matches: boolean): UiNode[] {
    if (!matches || (flash.text === "" && !flash.running)) {
        return [];
    }
    const nodes: UiNode[] = [
        { type: "steps", steps: say.flashSteps, current: Math.min(stepOf(flash.stage), say.flashSteps.length), failed: flash.failed },
    ];
    if (flash.running) {
        nodes.push({ type: "progress", title: flash.text, value: flash.fraction });
    } else {
        nodes.push({ type: "text", text: flash.text, tone: flash.failed ? "bad" : "good" });
    }
    return nodes;
}

function nearbyText(board: NearbyBoard): string {
    if (board.connected) {
        return say.inUse;
    }
    if (board.paired) {
        return say.paired;
    }
    if (board.busy) {
        return say.busyElsewhere;
    }
    return board.known && board.keyed ? say.notPaired : say.noKey;
}

function bluetoothNodes(): UiNode[] {
    const radio = radioNow();
    const nodes: UiNode[] = [{ type: "text", text: say.bluetoothIntro, style: "caption", tone: "muted" }];
    if (radio.state === "missing" || radio.state === "off") {
        nodes.push({ type: "text", text: radio.stateText, tone: "warn" });
        nodes.push(...noteNode(bluetoothNote));
        return nodes;
    }
    const busy = scan.running || pairing.running;
    for (const board of radio.boards) {
        nodes.push({
            type: "deviceItem",
            title: board.name,
            subtitle: [board.chip, board.address, board.signalText].filter((part) => part !== "").join(" · "),
            icon: "bluetooth",
            state: board.connected || board.paired ? "ready" : board.busy ? "busy" : "offline",
            stateText: nearbyText(board),
        });
        if (board.paired) {
            nodes.push({
                type: "row",
                children: [{ type: "button", id: "unpair:" + board.address, title: say.forgetPairing, enabled: !busy }],
            });
        } else if (board.known && board.keyed) {
            nodes.push({
                type: "row",
                children: [{ type: "button", id: "pair:" + board.address, title: say.pair, style: "primary", enabled: !busy }],
            });
        }
    }
    const nearby = new Set(radio.boards.map((board) => board.address));
    for (const board of radio.paired) {
        if (nearby.has(board.address)) {
            continue;
        }
        nodes.push({
            type: "deviceItem",
            title: board.name !== "" ? board.name : board.id,
            subtitle: board.address,
            icon: "bluetooth",
            state: "offline",
            stateText: say.notNearby,
        });
        nodes.push({
            type: "row",
            children: [{ type: "button", id: "unpair:" + board.address, title: say.forgetPairing, enabled: !busy }],
        });
    }
    if (scanned && !scan.running && radio.boards.length === 0) {
        nodes.push({ type: "text", text: say.noneNearby, tone: "muted" });
    }
    if (radio.boards.some((board) => !board.paired && !(board.known && board.keyed))) {
        nodes.push({ type: "text", text: say.needsKey, style: "caption", tone: "muted" });
    }
    nodes.push(...workNodes(scan));
    if (pairing.device !== shownBoard) {
        nodes.push(...workNodes(pairing));
    }
    nodes.push({
        type: "row",
        children: [{ type: "button", id: "bleScan", title: say.lookForBoards, icon: "bluetooth", enabled: !busy }],
    });
    nodes.push(...noteNode(bluetoothNote));
    return nodes;
}

function renderMain(): void {
    if (!mainShown) {
        return;
    }
    const listed = boardsNow();
    const ports = portsNow();
    const firmware = safely(() => esp32().firmware(), null);
    if (chosenPort === "" || !ports.some((port) => port.name === chosenPort)) {
        const likely = ports.find((port) => port.likely);
        chosenPort = likely !== undefined ? likely.name : ports.length > 0 ? ports[0].name : "";
    }
    const boardNodes: UiNode[] = listed.map((board): UiNode => ({
        type: "deviceItem",
        id: "board:" + board.id,
        title: board.name,
        subtitle: board.summary,
        icon: "svg:icons/board.svg",
        state: board.state,
        stateText: board.stateText,
    }));
    if (boardNodes.length === 0) {
        const problem = safely(() => esp32().problem(), "");
        boardNodes.push({ type: "text", text: problem !== "" ? problem : say.noBoards, tone: problem !== "" ? "warn" : "muted" });
    }
    boardNodes.push({ type: "row", children: [{ type: "button", id: "rescan", title: say.rescan, icon: "refresh-cw" }] });
    const firmwareNodes: UiNode[] = [
        { type: "keyValue", rows: [{ key: say.bundled, value: firmware !== null ? firmware.version : say.noFirmware }] },
    ];
    if (ports.length > 0) {
        firmwareNodes.push({
            type: "choice",
            id: "port",
            title: say.port,
            options: ports.map((port) => ({ id: port.name, title: port.title })),
            value: chosenPort,
            enabled: !flash.running,
        });
    } else {
        firmwareNodes.push({ type: "text", text: say.noPorts, tone: "warn" });
    }
    firmwareNodes.push({
        type: "row",
        children: [
            {
                type: "button",
                id: "flash",
                title: say.flash,
                style: "primary",
                enabled: !flash.running && chosenPort !== "" && firmware !== null,
            },
        ],
    });
    firmwareNodes.push(...flashNodes(true));
    firmwareNodes.push({ type: "text", text: say.bootHint, style: "caption", tone: "muted" });
    main.render({
        type: "column",
        children: [
            { type: "text", text: say.intro, style: "caption", tone: "muted" },
            { type: "group", title: say.boards, children: boardNodes },
            { type: "group", title: say.bluetoothGroup, children: bluetoothNodes() },
            { type: "group", title: say.firmware, children: firmwareNodes },
            { type: "text", text: say.help, style: "caption", tone: "muted" },
        ],
    });
}

function firmwareLine(board: Board): string {
    if (board.firmware === null) {
        return board.legacy ? say.oldFirmware : "";
    }
    if (board.upgradable && board.bundled !== null) {
        return filled(say.available, board.firmware, board.bundled);
    }
    return board.bundled !== null && board.firmware === board.bundled ? filled(say.upToDate, board.firmware) : board.firmware;
}

function whereText(board: Board): string {
    if (board.bluetooth) {
        const where = say.viaBluetooth + " " + board.where;
        return board.signalText !== "" ? where + " · " + board.signalText : where;
    }
    return board.wifi ? "Wi-Fi " + board.where : board.where;
}

function bluetoothSheetNodes(board: Board, usable: boolean): UiNode[] {
    const nodes: UiNode[] = [];
    if (board.bluetooth) {
        nodes.push({ type: "text", text: say.bluetoothLinked, style: "caption", tone: "muted" });
        nodes.push({
            type: "row",
            children: [{ type: "button", id: "unpair", title: say.forgetPairing, style: "danger", enabled: !pairing.running }],
        });
    } else if (board.wifi) {
        nodes.push({ type: "text", text: say.pairSerialOnly, style: "caption", tone: "muted" });
    } else if (board.legacy || (board.chip !== null && !board.hasBluetoothLink)) {
        nodes.push({ type: "text", text: say.noBluetoothLink, style: "caption", tone: "muted" });
    } else {
        nodes.push({ type: "text", text: say.pairIntro, style: "caption", tone: "muted" });
        nodes.push({
            type: "row",
            children: [
                {
                    type: "button",
                    id: "pair",
                    title: say.pairBoard,
                    icon: "bluetooth",
                    style: "primary",
                    enabled: usable && !pairing.running,
                },
            ],
        });
    }
    if (pairing.device === board.id) {
        nodes.push(...workNodes(pairing));
    }
    nodes.push(...noteNode(pairNote));
    return nodes;
}

function renderBoard(): void {
    if (shownBoard === "") {
        return;
    }
    const board = boardsNow().find((each) => each.id === shownBoard);
    if (board === undefined) {
        sheet.render({ type: "column", children: [{ type: "text", text: say.gone, tone: "warn" }] });
        return;
    }
    const usable = board.connected && !board.held;
    const rows: { key: string; value: string }[] = [];
    if (board.chip !== null) {
        rows.push({ key: say.chip, value: board.chip });
    }
    const version = firmwareLine(board);
    if (version !== "") {
        rows.push({ key: say.version, value: version });
    }
    rows.push({ key: say.where, value: whereText(board) });
    if (board.phone !== "") {
        rows.push({ key: say.phone, value: board.phone });
    }
    if (board.wifiText !== "") {
        rows.push({ key: say.wifi, value: board.wifiText });
    }
    if (board.parts !== "") {
        rows.push({ key: say.parts, value: board.parts });
    }
    if (board.roundTripMs !== null) {
        rows.push({ key: say.roundTrip, value: filled(say.milliseconds, board.roundTripMs.toFixed(1)) });
    }
    if (board.bluetooth && board.slowTripMs !== null) {
        rows.push({ key: say.slowTrip, value: filled(say.milliseconds, board.slowTripMs.toFixed(1)) });
    }
    const children: UiNode[] = [{ type: "keyValue", rows }];
    if (board.stateText !== "" && board.state !== "ready") {
        children.push({ type: "text", text: board.stateText, tone: board.state === "problem" ? "warn" : "muted" });
    }
    if (board.slowBluetooth && board.slowText !== "") {
        children.push({ type: "text", text: board.slowText, tone: "warn" });
    }
    if (board.problem !== "") {
        children.push({ type: "text", text: board.problem, style: "caption", tone: "warn" });
    }
    children.push({
        type: "row",
        children: [
            { type: "button", id: "tap", title: say.tryIt, icon: "tap-echo", enabled: usable },
            { type: "button", id: "disconnect", title: say.disconnect, enabled: !board.held },
        ],
    });
    children.push(...noteNode(tapNote));
    const flashing = flash.running && (flash.device === board.id || flash.port === board.where);
    const firmwareChildren: UiNode[] = [];
    if (board.via !== "serial") {
        firmwareChildren.push({ type: "text", text: say.flashSerialOnly, style: "caption", tone: "muted" });
    } else {
        firmwareChildren.push({
            type: "button",
            id: "flash",
            title: board.upgradable ? say.upgrade : say.flash,
            style: board.upgradable || board.legacy ? "primary" : "normal",
            enabled: !board.held && !flashing,
        });
    }
    firmwareChildren.push(...flashNodes(flash.device === board.id || flash.port === board.where));
    children.push({ type: "group", title: say.firmware, children: firmwareChildren });
    const wifiChildren: UiNode[] = [];
    if (board.via !== "serial") {
        wifiChildren.push({ type: "text", text: say.wifiSerialOnly, style: "caption", tone: "muted" });
    } else if (!board.hasWifi) {
        wifiChildren.push({ type: "text", text: board.legacy ? say.wifiSerialOnly : say.wifiMissing, style: "caption", tone: "muted" });
    } else {
        wifiChildren.push({ type: "text", text: say.wifiIntro, style: "caption", tone: "muted" });
        wifiChildren.push({ type: "field", id: "ssid", title: say.ssid, value: ssid, enabled: usable });
        wifiChildren.push({ type: "field", id: "password", title: say.password, value: password, enabled: usable });
        const buttons: UiNode[] = [{ type: "button", id: "wifiSave", title: say.saveWifi, style: "primary", enabled: usable }];
        if (board.wifiConfigured) {
            buttons.push({ type: "button", id: "wifiForget", title: say.forgetWifi, enabled: usable });
        }
        wifiChildren.push({ type: "row", children: buttons });
    }
    wifiChildren.push(...noteNode(wifiNote));
    children.push({ type: "group", title: say.wifiGroup, children: wifiChildren });
    children.push({ type: "group", title: say.bluetoothGroup, children: bluetoothSheetNodes(board, usable) });
    if (!board.legacy && board.boardName !== null) {
        children.push({
            type: "group",
            title: say.nameGroup,
            children: [
                { type: "field", id: "name", title: say.name, value: boardName !== "" ? boardName : board.boardName, enabled: usable },
                { type: "row", children: [{ type: "button", id: "rename", title: say.rename, enabled: usable }] },
                ...noteNode(renameNote),
            ],
        });
    }
    const diagnosisChildren: UiNode[] = [{ type: "button", id: "diagnose", title: say.diagnose, enabled: !board.held }];
    if (diagnosis !== null && diagnosis.device === board.id) {
        diagnosisChildren.push({ type: "text", text: diagnosis.text, tone: diagnosis.ok ? "good" : "bad" });
        if (diagnosis.rows.length > 0) {
            diagnosisChildren.push({ type: "keyValue", rows: diagnosis.rows });
        }
    }
    children.push({ type: "group", title: say.diagnosisGroup, children: diagnosisChildren });
    children.push({ type: "text", text: say.help, style: "caption", tone: "muted" });
    sheet.render({ type: "column", children });
}

function renderAll(): void {
    renderMain();
    renderBoard();
}

async function startFlash(device: string): Promise<void> {
    if (device === "" || flash.running) {
        return;
    }
    flash = { device, port: device, running: true, fraction: 0, stage: "preparing", text: "", failed: false };
    renderAll();
    try {
        await esp32().flash({ device }).result;
    } catch (error) {
        if (flash.running) {
            flash = { ...flash, running: false, failed: true, text: messageOf(error) };
            renderAll();
        }
    }
}

async function runNote(work: () => ExtensionJob<StoreValue>, done: string): Promise<Note> {
    try {
        await work().result;
        return { text: done, failed: false };
    } catch (error) {
        return { text: messageOf(error), failed: true };
    }
}

async function startScan(): Promise<void> {
    if (scan.running || pairing.running) {
        return;
    }
    scan = { device: "", running: true, fraction: 0, text: "" };
    bluetoothNote = null;
    renderMain();
    try {
        await esp32()
            .scan({})
            .onProgress((fraction, text) => {
                if (scan.running) {
                    scan = { device: "", running: true, fraction, text };
                    renderMain();
                }
            }).result;
    } catch (error) {
        bluetoothNote = { text: messageOf(error), failed: true };
    }
    scan = idle;
    scanned = true;
    renderMain();
}

async function runPairing(device: string, work: () => ExtensionJob<StoreValue>, done: string): Promise<Note | null> {
    if (pairing.running || scan.running) {
        return null;
    }
    pairing = { device, running: true, fraction: -1, text: "" };
    renderAll();
    let note: Note;
    try {
        await work().onProgress((fraction, text) => {
            if (pairing.running && pairing.device === device) {
                pairing = { device, running: true, fraction, text };
                renderAll();
            }
        }).result;
        note = { text: done, failed: false };
    } catch (error) {
        note = { text: messageOf(error), failed: true };
    }
    pairing = idle;
    return note;
}

main.on("open", () => {
    mainShown = true;
    renderMain();
});

main.on("close", () => {
    mainShown = false;
});

main.on("change", ({ id, value }) => {
    if (id === "port" && typeof value === "string") {
        chosenPort = value;
        renderMain();
    }
});

main.on(
    "click",
    async ({ id }) => {
        if (id === "rescan") {
            safely(() => esp32().rescan(), false);
            renderMain();
        } else if (id === "flash") {
            await startFlash(chosenPort);
        } else if (id === "bleScan") {
            await startScan();
        } else if (id.startsWith("pair:") || id.startsWith("unpair:")) {
            const pairs = id.startsWith("pair:");
            const device = id.substring(id.indexOf(":") + 1);
            const note = await runPairing(
                device,
                () => (pairs ? esp32().pair({ device }) : esp32().unpair({ device })),
                pairs ? say.pairedDone : say.unpaired,
            );
            if (note !== null) {
                bluetoothNote = note;
            }
            renderAll();
        } else if (id.startsWith("board:")) {
            const board = boardsNow().find((each) => "board:" + each.id === id);
            if (board !== undefined && board.via === "serial") {
                chosenPort = board.where;
                renderMain();
            }
        }
    },
    { again: "parallel" },
);

sheet.on("open", ({ id }) => {
    shownBoard = id;
    ssid = "";
    password = "";
    boardName = "";
    tapNote = null;
    wifiNote = null;
    renameNote = null;
    pairNote = null;
    renderBoard();
});

sheet.on("close", () => {
    shownBoard = "";
    password = "";
});

sheet.on("change", ({ id, value }) => {
    if (typeof value !== "string") {
        return;
    }
    if (id === "ssid") {
        ssid = value;
    } else if (id === "password") {
        password = value;
    } else if (id === "name") {
        boardName = value;
    }
});

sheet.on(
    "click",
    async ({ id }) => {
        const device = shownBoard;
        if (device === "") {
            return;
        }
        if (id === "tap") {
            const tapped = safely(() => esp32().tap(device), false);
            tapNote = { text: tapped ? say.tapped : say.notTapped, failed: !tapped };
        } else if (id === "disconnect") {
            safely(() => esp32().disconnect(device), false);
        } else if (id === "flash") {
            await startFlash(device);
            return;
        } else if (id === "wifiSave") {
            wifiNote = await runNote(() => esp32().wifi({ device, ssid: ssid.trim(), password }), say.wifiSent);
            if (!wifiNote.failed) {
                password = "";
            }
        } else if (id === "wifiForget") {
            wifiNote = await runNote(() => esp32().wifi({ device, ssid: "", password: "" }), say.wifiForgotten);
        } else if (id === "rename") {
            renameNote = await runNote(() => esp32().rename({ device, name: boardName.trim() }), say.renamed);
        } else if (id === "pair" || id === "unpair") {
            const note = await runPairing(
                device,
                () => (id === "pair" ? esp32().pair({ device }) : esp32().unpair({ device })),
                id === "pair" ? say.pairedDone : say.unpaired,
            );
            if (note !== null) {
                pairNote = note;
            }
            renderAll();
            return;
        } else if (id === "diagnose") {
            diagnosis = { device, ok: true, text: "…", rows: [] };
            renderBoard();
            try {
                await esp32().diagnose({ device }).result;
            } catch (error) {
                if (diagnosis !== null && diagnosis.device === device && diagnosis.text === "…") {
                    diagnosis = { device, ok: false, text: messageOf(error), rows: [] };
                }
            }
        }
        renderBoard();
    },
    { again: "parallel" },
);

for (const topic of ["board.added", "board.changed", "board.removed", "ports.changed", "bluetooth.changed"]) {
    events.on(topic, () => renderAll(), { again: "queue" });
}

events.on(
    "flash.progress",
    ({ value }) => {
        const fields = fieldsOf(value);
        flash = {
            device: textIn(fields, "device"),
            port: textIn(fields, "port"),
            running: true,
            fraction: numberIn(fields, "fraction"),
            stage: textIn(fields, "stage"),
            text: textIn(fields, "text"),
            failed: false,
        };
        renderAll();
    },
    { again: "queue" },
);

events.on(
    "flash.done",
    ({ value }) => {
        const fields = fieldsOf(value);
        const ok = fields["ok"] === true;
        flash = {
            device: textIn(fields, "device"),
            port: textIn(fields, "port") !== "" ? textIn(fields, "port") : flash.port,
            running: false,
            fraction: ok ? 1 : flash.fraction,
            stage: ok ? "done" : flash.stage,
            text: textIn(fields, "text"),
            failed: !ok,
        };
        renderAll();
    },
    { again: "queue" },
);

events.on(
    "board.diagnosed",
    ({ value }) => {
        const fields = fieldsOf(value);
        const ok = fields["ok"] === true;
        const problem = textIn(fields, "problem");
        const rows: { key: string; value: string }[] = [];
        const chip = textIn(fields, "chip");
        if (chip !== "") {
            rows.push({ key: say.chip, value: chip });
        }
        const firmware = textIn(fields, "firmware");
        if (firmware !== "") {
            rows.push({ key: say.version, value: firmware });
        }
        const parts = textIn(fields, "parts");
        if (parts !== "") {
            rows.push({ key: say.parts, value: parts });
        }
        diagnosis = { device: textIn(fields, "device"), ok: ok && problem === "", text: problem !== "" ? problem : say.healthy, rows };
        renderBoard();
    },
    { again: "queue" },
);
