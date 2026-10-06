/*
 * ホストとの BLE 接続間隔が希望（CONFIG_BT_PERIPHERAL_PREF_MIN_INT〜MAX_INT）から外れたら、
 * 接続パラメータの更新を自分から要求し直す。右手（ホストに対してペリフェラル役）専用。
 * 有効化は CONFIG_ROBA_BLE_CONN_PARAM_RETRY（Kconfig と config/roBa_R.conf を参照）。
 * 入れた理由（症状と実測日）は config/roBa_R.conf に書いてある。ここには仕組みと回数・待ち時間を書く。
 *
 * Zephyr 4.1 の BLE スタックの仕組み（zmkfirmware/zephyr@10ba6d0 の subsys/bluetooth/host/ を読んだ結果）:
 *   - ペリフェラル役の接続では、接続の CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT（ms、既定 5000）後に 1 回だけ
 *     PPCP（CONFIG_BT_PERIPHERAL_PREF_*）での更新を自動で要求する（conn.c:1242 で予約、deferred_work で送る）。
 *     受け入れられたらそれで終わりで、その後ホストが間隔を変えても要求し直さない。
 *     断られたときは、理由が「Unsupported LL Parameter Value (0x20)」の場合に限り、
 *     CONFIG_BT_CONN_PARAM_RETRY_TIMEOUT（既定 5000 ms）おきに CONFIG_BT_CONN_PARAM_RETRY_COUNT（既定 3）回まで
 *     PPCP を送り直す（hci_core.c:1963-1969、conn.c:2270-2288）
 *   - Zephyr がやり直すかどうかは、どの要求が断られたかではなく、接続の AUTO_UPDATE フラグと残り回数で決まる。
 *     フラグは PPCP を送ったときに立ち、受け入れ、0x20 以外の拒否、残り回数 0 での 0x20 の拒否で消える
 *     （conn.c:2280-2283、hci_core.c:1961-1972）。このファイルの要求（bt_conn_le_param_update →
 *     send_conn_le_param_update、conn.c:3540-3552, 2082-2117）はフラグに触らない。
 *     そのため、Zephyr の自動要求が 0x20 で断られてやり直しが残っている間は、このファイルの要求が 0x20 で
 *     断られると Zephyr のやり直しの 1 回として数えられ、Zephyr のやり直しの予約が無ければ新しく予約される。
 *     Zephyr のやり直しとこのファイルの要求は交互に出て、Zephyr のやり直しはその分早く尽きる。
 *     要求の合計は、Zephyr の最大 1 + CONFIG_BT_CONN_PARAM_RETRY_COUNT 回（既定 4 回）とこのファイルの
 *     MAX_REQUESTS 回を超えない。2 つが重なるとコントローラが後から送った方を断る。このファイルなら -EACCES
 *     などが返り、送れなかった要求として数える。Zephyr なら「Send auto LE param update failed」の WRN が出て、
 *     Zephyr はその回のやり直しを予約しない（conn.c:2284-2287）
 *   - Zephyr のやり直しが全部終わるまで（既定で接続から 5 + 3 × 5 + 1 = 21 s）待てば重ならないが、
 *     Windows で 15 ms が残ったときに直るのがその分遅れる。重なっても回数は上の上限で止まるので、
 *     待つのは 1 回目の自動要求まで（AUTO_REQUEST_WAIT_MS）にしている
 *   - Zephyr の自動要求が通った後や 0x20 以外で断られた後、やり直しを使い切った後は、このファイルの要求が
 *     断られても Zephyr はやり直さない。そちらはこのファイルの RETRY_DELAY_MS 後の再要求が受け持つ
 *   - LL の更新の結果（hci_core.c le_conn_update_complete）は、L2CAP へ切り替える最初の 1 回
 *     （0x1A Unsupported Remote Feature。ホストが LL の手順に対応しない）だけは何も通知せずに L2CAP で
 *     要求し直し、それ以外はどの分岐でも最後に notify_le_param_updated を呼ぶ（hci_core.c:1924-1977）。
 *     受け入れなら新しい間隔、断られたなら変わらなかった今の間隔が le_param_updated に渡る
 *   - L2CAP で要求したときの返事はログに出るだけで、何も通知されない（l2cap.c:1042-1052 le_conn_param_rsp）。
 *     INITIAL_CHECK_MS と RECHECK_DELAY_MS の後の確認があるのはこのため
 *   - bt_conn_le_param_update() は、自動要求のタイマーが発火済みなら即送信し、まだなら値を保存して
 *     タイマーに送らせる（conn.c:3540-3552）。保存した値で送られると AUTO_UPDATE フラグが立たず、
 *     Zephyr 自身の 0x20 のやり直しが無くなる（conn.c:2254-2266）。そのため自動要求より前には呼ばない
 *
 * このファイルのやること:
 *   - le_param_updated で間隔が希望の範囲外なら、RETRY_DELAY_MS 後に確かめて、外れたままなら要求する
 *   - 接続の INITIAL_CHECK_MS 後にも 1 回確かめる（自動要求が L2CAP で黙って断られたときの保険）
 *   - 接続から AUTO_REQUEST_WAIT_MS たつまでは要求しない（Zephyr の自動要求を先に出させる）。
 *     その前に確かめる時刻が来たら、回数に数えず、ログも DBG だけでそこまで待つ
 *   - 実際に送れた要求（戻り値 0）は 1 接続あたり MAX_REQUESTS 回まで。送れなかったとき（他の LL の手順の
 *     最中にコントローラが断る -EACCES など）は WRN を出して RETRY_DELAY_MS 後にやり直し、別に
 *     MAX_FAILED_SUBMITS 回まで数える。どちらかの上限に達したら WRN を 1 回出して諦める。
 *     上限を置くのは、7.5 ms を断るホスト（Apple の機器など）と押し問答を続けないため
 *   - 左手との接続（右手がセントラル役）など、ペリフェラル役でない接続は何もしない。
 *     左手との間隔は ZMK の split central.c が接続時に指定している
 *
 * 動く場所:
 *   connected と le_param_updated は BT の受信ワークキュー、disconnected とこのファイルのワークは
 *   システムワークキューで動く（Zephyr は disconnected を deferred_work から呼ぶ）。
 *   同じ送信処理（send_conn_le_param_update）を Zephyr 自身も deferred_work（システムワークキュー）から
 *   呼んでいる（conn.c:2263, 2280。bt_hci_cmd_send_sync はシステムワークキューから呼ばれるとコマンドを
 *   その場で処理する、hci_core.c:410）。
 *   slot->conn と slot->connected_at は 2 つのスレッドから触るのでスピンロックで守り、
 *   ワークは自分で取った参照で動く。
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 間隔のずれに気づいてから確かめるまでの待ち（ms）。ホスト側の手順（暗号化や PHY の更新）が終わるのを待つ */
#define RETRY_DELAY_MS 2000
/* 接続後に 1 回だけ間隔を確かめるまでの待ち（ms）。Zephyr の自動要求（5 s 後）とその返事を待ってから */
#define INITIAL_CHECK_MS 10000
/* 要求を出した後、返事の通知が来ないときに結果を確かめるまでの待ち（ms） */
#define RECHECK_DELAY_MS 8000
/* 接続からこの時間（ms）がたつまでは要求しない。Zephyr の自動要求（CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT 後）に
 * 1 s の余裕を足した値。CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT は、Kconfig の depends に入れた
 * BT_GAP_PERIPHERAL_PREF_PARAMS と同じ「if BT_CONN」の中にあり、条件なしで既定値を持つので、
 * このファイルがビルドされるときは必ず定義されている（zephyr subsys/bluetooth/host/Kconfig:328, 759, 776） */
#define AUTO_REQUEST_WAIT_MS (CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT + 1000)
/* 1 接続あたり、実際に送れた要求の上限 */
#define MAX_REQUESTS 3
/* 1 接続あたり、送れなかった要求の上限（ずっと送れないときにやり直しを止めるため） */
#define MAX_FAILED_SUBMITS 6

/* 希望の間隔（1.25 ms 単位）。Zephyr の自動要求と同じ値を使う */
#define PREF_MIN CONFIG_BT_PERIPHERAL_PREF_MIN_INT
#define PREF_MAX CONFIG_BT_PERIPHERAL_PREF_MAX_INT

struct conn_slot {
    struct bt_conn *conn;         /* 追跡中の接続（参照を 1 つ持つ）。NULL なら未使用 */
    struct k_work_delayable work; /* 間隔の確認と要求（システムワークキュー） */
    int64_t connected_at;         /* 接続した時刻（k_uptime_get の ms） */
    uint8_t requests;             /* この接続で実際に送れた要求の数 */
    uint8_t failed;               /* この接続で送れなかった要求の数 */
    bool gave_up;                 /* 上限に達して諦めた（WRN は 1 回だけ） */
};

/* bt_conn_index() で引く。要素数は Zephyr の接続オブジェクトの数（右手は 6）と同じ */
static struct conn_slot slots[CONFIG_BT_MAX_CONN];
/* BT 受信ワークキュー（connected / le_param_updated）とシステムワークキュー（disconnected / ワーク）の間で
 * slot->conn と slot->connected_at を守る */
static struct k_spinlock lock;

static inline bool interval_preferred(uint16_t interval) {
    return interval >= PREF_MIN && interval <= PREF_MAX;
}

static void log_addr(const struct bt_conn *conn, char *buf, size_t len) {
    bt_addr_le_to_str(bt_conn_get_dst(conn), buf, len);
}

/* 追跡中の接続ならその slot を返す。左手との接続（セントラル役）などは追跡していないので NULL */
static struct conn_slot *slot_if_tracked(struct bt_conn *conn) {
    struct conn_slot *slot = &slots[bt_conn_index(conn)];

    k_spinlock_key_t key = k_spin_lock(&lock);
    bool tracked = slot->conn == conn;
    k_spin_unlock(&lock, key);

    return tracked ? slot : NULL;
}

/* 間隔を確かめ、希望から外れていれば要求する（システムワークキュー） */
static void check_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct conn_slot *slot = CONTAINER_OF(dwork, struct conn_slot, work);
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;
    struct bt_conn *conn;
    const struct bt_le_conn_param *param;
    int64_t connected_at;
    int64_t elapsed;
    int err;

    /* 切断処理と前後しても、自分の参照を取り終えるまで conn が手放されないようにする */
    k_spinlock_key_t key = k_spin_lock(&lock);
    conn = slot->conn ? bt_conn_ref(slot->conn) : NULL;
    connected_at = slot->connected_at;
    k_spin_unlock(&lock, key);
    if (!conn) {
        return;
    }

    if (bt_conn_get_info(conn, &info) < 0 || info.state != BT_CONN_STATE_CONNECTED) {
        goto done;
    }

    log_addr(conn, addr, sizeof(addr));

    if (interval_preferred(info.le.interval)) {
        LOG_DBG("%s: interval %u (x1.25 ms) is within %u..%u, nothing to do", addr,
                info.le.interval, PREF_MIN, PREF_MAX);
        goto done;
    }

    /* Zephyr の自動要求がまだなら、それを先に出させる。ここで要求すると値が保存されて自動要求の代わりに
     * 送られ、Zephyr 自身の 0x20 のやり直しが無くなる。回数には数えず、INF も出さない */
    elapsed = k_uptime_get() - connected_at;
    if (elapsed < AUTO_REQUEST_WAIT_MS) {
        unsigned int wait_ms = (unsigned int)(AUTO_REQUEST_WAIT_MS - elapsed);

        LOG_DBG("%s: interval %u (x1.25 ms) is outside %u..%u; waiting %u ms for the stack's own request",
                addr, info.le.interval, PREF_MIN, PREF_MAX, wait_ms);
        k_work_schedule(&slot->work, K_MSEC(wait_ms));
        goto done;
    }

    if (slot->requests >= MAX_REQUESTS || slot->failed >= MAX_FAILED_SUBMITS) {
        if (!slot->gave_up) {
            slot->gave_up = true;
            LOG_WRN("%s: giving up after %u requests (%u not sent), interval stays %u (x1.25 ms = %u us)",
                    addr, slot->requests, slot->failed, info.le.interval,
                    BT_CONN_INTERVAL_TO_US(info.le.interval));
        }
        goto done;
    }

    param = BT_LE_CONN_PARAM(PREF_MIN, PREF_MAX, CONFIG_BT_PERIPHERAL_PREF_LATENCY,
                             CONFIG_BT_PERIPHERAL_PREF_TIMEOUT);
    err = bt_conn_le_param_update(conn, param);
    if (err) {
        /* 送れなかった（他の LL の手順の最中で -EACCES など）。要求の回数には数えず、少し待ってやり直す */
        slot->failed++;
        LOG_WRN("%s: request not sent (err %d, %u/%u); retrying in %u ms", addr, err, slot->failed,
                MAX_FAILED_SUBMITS, RETRY_DELAY_MS);
        k_work_schedule(&slot->work, K_MSEC(RETRY_DELAY_MS));
        goto done;
    }

    slot->requests++;
    LOG_INF("%s: request %u/%u: interval %u -> %u..%u (x1.25 ms), latency %u, timeout %u", addr,
            slot->requests, MAX_REQUESTS, info.le.interval, PREF_MIN, PREF_MAX,
            CONFIG_BT_PERIPHERAL_PREF_LATENCY, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT);

    /* 返事が来れば le_param_updated が先に動く（受け入れなら範囲内で終わり、拒否なら RETRY_DELAY_MS 後に次）。
     * 返事の通知が来なくても RECHECK_DELAY_MS 後に結果を確かめる。
     * le_param_updated が先に予約していればそちら（短い方）を残す */
    k_work_schedule(&slot->work, K_MSEC(RECHECK_DELAY_MS));

done:
    bt_conn_unref(conn);
}

static void connected(struct bt_conn *conn, uint8_t err) {
    char addr[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    if (err) {
        return;
    }

    if (bt_conn_get_info(conn, &info) < 0 || info.type != BT_CONN_TYPE_LE ||
        info.role != BT_CONN_ROLE_PERIPHERAL) {
        /* 左手との接続（右手がセントラル役）は対象外 */
        return;
    }

    struct conn_slot *slot = &slots[bt_conn_index(conn)];

    /* 参照を持つことで、切断の通知を処理して手放すまでこの接続オブジェクトが再利用されず、
     * bt_conn_index との対応が崩れないようにする */
    struct bt_conn *ref = bt_conn_ref(conn);
    if (!ref) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    struct bt_conn *old = slot->conn;
    slot->conn = ref;
    slot->connected_at = k_uptime_get();
    slot->requests = 0;
    slot->failed = 0;
    slot->gave_up = false;
    k_spin_unlock(&lock, key);

    if (old) {
        /* 通常は起きない（切断の通知で手放している）。念のため古い参照を返す */
        bt_conn_unref(old);
    }

    log_addr(conn, addr, sizeof(addr));
    LOG_DBG("%s: connected as peripheral, interval %u (x1.25 ms); check in %u ms", addr,
            info.le.interval, INITIAL_CHECK_MS);

    /* 接続後の一回きりの確認。間隔のずれが先に通知されれば le_param_updated が前倒しする */
    k_work_reschedule(&slot->work, K_MSEC(INITIAL_CHECK_MS));
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    struct conn_slot *slot = &slots[bt_conn_index(conn)];

    k_spinlock_key_t key = k_spin_lock(&lock);
    bool tracked = slot->conn == conn;
    if (tracked) {
        slot->conn = NULL;
    }
    k_spin_unlock(&lock, key);
    if (!tracked) {
        return;
    }

    /* ワークと同じシステムワークキューで動くので、ここでワークが同時に走っていることはない。
     * 予約だけ取り消す。取り消しが遅れて走っても、ワーク側は参照を取れなければ何もしない */
    k_work_cancel_delayable(&slot->work);

    LOG_DBG("conn %u: disconnected (reason 0x%02x) after %u requests (%u not sent)",
            bt_conn_index(conn), reason, slot->requests, slot->failed);

    /* connected で取った参照を返す */
    bt_conn_unref(conn);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    char addr[BT_ADDR_LE_STR_LEN];
    struct conn_slot *slot = slot_if_tracked(conn);
    if (!slot) {
        return;
    }

    log_addr(conn, addr, sizeof(addr));

    if (interval_preferred(interval)) {
        LOG_DBG("%s: interval %u (x1.25 ms) latency %u timeout %u: within %u..%u", addr, interval,
                latency, timeout, PREF_MIN, PREF_MAX);
        return;
    }

    /* ホストが間隔を変えた、または自分（か Zephyr の自動要求）の要求が断られた（その場合は変わらなかった
     * 今の間隔が渡る）。少し待ってから確かめる。自動要求より前なら待つこと、回数の上限と WRN はワーク側で扱う */
    LOG_INF("%s: interval %u (x1.25 ms = %u us) is outside %u..%u; checking again in %u ms", addr,
            interval, BT_CONN_INTERVAL_TO_US(interval), PREF_MIN, PREF_MAX, RETRY_DELAY_MS);
    k_work_reschedule(&slot->work, K_MSEC(RETRY_DELAY_MS));
}

/* ZMK 本体の ble.c / central.c の登録（bt_conn_cb_register）とは別に、リンカのセクションで登録する。
 * Zephyr は両方の登録先に通知する（conn.c notify_connected / notify_le_param_updated） */
BT_CONN_CB_DEFINE(roba_ble_conn_param_retry) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
};

static int roba_ble_conn_param_retry_init(void) {
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        k_work_init_delayable(&slots[i].work, check_work_handler);
    }
    return 0;
}

/* BT が有効になる（ZMK の APPLICATION レベル、CONFIG_ZMK_BLE_INIT_PRIORITY=50）より前にワークを初期化する */
SYS_INIT(roba_ble_conn_param_retry_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
