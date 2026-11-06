package com.radiometer.remotedesk;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.content.Context;
import android.graphics.Path;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;

/**
 * 无障碍手势注入：把浏览器上的点击/滑动/长按/返回/主页翻译成系统手势。
 *
 * 未在系统设置里开启时 sInstance == null，所有静态入口返回 false，上层据此
 * 明确报错（WS 回包 input_status），绝不静默丢弃事件。
 * 开启路径：设置 → 无障碍 → 已下载的服务 / 已安装的应用 → RemoteDesk 输入服务。
 */
public class InputAccessibilityService extends AccessibilityService {
    private static final String TAG = "RemoteDesk";

    /** 长按判定阈值：按住不动达到该时长即按长按注入（系统 LongPress 超时约 500ms）。 */
    public static final int LONG_PRESS_MS = 600;

    /** 与 C++ 侧 InputManager 约定一致的全局动作码。 */
    private static final int ACTION_BACK = 1;
    private static final int ACTION_HOME = 2;

    private static volatile InputAccessibilityService sInstance;

    public static InputAccessibilityService getInstance() {
        return sInstance;
    }

    public static boolean isReady() {
        return sInstance != null;
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    // 键盘写入的本地影子文本。ACTION_SET_TEXT 只是把请求异步投递给目标应用，
    // 返回 true 时文本往往还没落地；连续快速按键时 node.getText() 会滞后，
    // 真机实测 150ms 连打 "Hello" 会丢字成 "ahlo"。这里以本服务最后一次写入的
    // 内容为准累积，目标节点追上影子后再以节点文本/选区为准（兼容手机端编辑）。
    private String shadowText = null;
    private String shadowKey = null;
    private int shadowSelStart = 0;
    private int shadowSelEnd = 0;
    private long shadowSentMs = 0;

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        sInstance = this;
        Log.i(TAG, "Accessibility input service connected");
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
    }

    @Override
    public void onInterrupt() {
    }

    @Override
    public void onDestroy() {
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }

    // ==================== 供 JNI 调用的静态入口 ====================
    // 全部先判 isReady()：未开启无障碍时返回 false，调用方必须把失败原因
    // 回给浏览器，而不是让事件消失。

    public static boolean tap(int x, int y) {
        final InputAccessibilityService service = sInstance;
        return service != null && service.performStroke(x, y, x, y, 40);
    }

    public static boolean swipe(int x1, int y1, int x2, int y2, int durationMs) {
        final InputAccessibilityService service = sInstance;
        return service != null
                && service.performStroke(x1, y1, x2, y2, Math.max(50, durationMs));
    }

    public static boolean longPress(int x, int y, int durationMs) {
        final InputAccessibilityService service = sInstance;
        return service != null
                && service.performStroke(x, y, x, y, Math.max(LONG_PRESS_MS, durationMs));
    }

    /**
     * 返回/主页。action 取值与 C++ 侧常量一致：1=BACK，2=HOME。
     * performGlobalAction 只能在主线程执行，这里 post 后再调用。
     */
    public static boolean globalAction(int action) {
        final InputAccessibilityService service = sInstance;
        if (service == null) {
            return false;
        }
        final int globalAction = (action == ACTION_HOME)
                ? GLOBAL_ACTION_HOME : GLOBAL_ACTION_BACK;
        service.mainHandler.post(() -> service.performGlobalAction(globalAction));
        return true;
    }

    /**
     * 浏览器按键 → 当前焦点输入框（英文/数字/回车/退格）。
     * 返回 0 = 没有可编辑的焦点控件（上层必须回执"先点输入框"）；
     *      1 = 已处理；
     *     -1 = 控件拒绝写入。
     * Android 没有跨应用键注入权限（INJECT_EVENTS 为 signature 级），IME 级注入
     * 又要额外实现输入法服务；无障碍 ACTION_SET_TEXT + ACTION_IME_ENTER 是
     * 不需要额外权限的最小可行路径，因此只对可编辑节点生效。
     */
    public static int inputKey(int keycode, String code, boolean shift,
                               boolean ctrl, boolean alt, boolean isChar) {
        final InputAccessibilityService service = sInstance;
        if (service == null) {
            return 0;
        }
        final AccessibilityNodeInfo node = service.findFocusedEditable();
        if (node == null) {
            return 0;
        }
        final boolean isBackspace = keycode == 8 || "Backspace".equals(code);
        final boolean isEnter = keycode == 13 || keycode == 10
                || "Enter".equals(code) || "NumpadEnter".equals(code);
        if (isBackspace) {
            return service.deleteBackward(node);
        }
        if (isEnter) {
            return service.pressEnter(node);
        }
        final String text;
        try {
            text = (isChar && keycode > 0)
                    ? new String(Character.toChars(keycode))
                    : charForCode(keycode, code, shift);
        } catch (Exception e) {
            // 非法的 Unicode 码点（isChar 携带的码点越界）：忽略而不是崩溃。
            Log.w(TAG, "invalid key code: " + keycode, e);
            return 1;
        }
        if (text == null) {
            // 方向键/功能键等不在最小方案范围：忽略而不是每次都弹失败提示。
            return 1;
        }
        return service.insertText(node, text);
    }

    /**
     * 浏览器输入法上屏的整段 Unicode 文本（中文/中英混排）→ 当前焦点输入框。
     * 与逐键输入共用同一份影子文本：连续多段上屏按到达顺序在当前光标处合并，
     * 不受 ACTION_SET_TEXT 异步落地延迟影响。
     * 返回 0 = 没有可编辑的焦点控件；1 = 已处理；-1 = 控件拒绝写入。
     */
    public static int inputText(String text) {
        final InputAccessibilityService service = sInstance;
        if (service == null) {
            return 0;
        }
        if (text == null || text.length() == 0) {
            return 1;
        }
        final AccessibilityNodeInfo node = service.findFocusedEditable();
        if (node == null) {
            return 0;
        }
        return service.insertText(node, text);
    }

    private AccessibilityNodeInfo findFocusedEditable() {
        try {
            AccessibilityNodeInfo root = getRootInActiveWindow();
            if (root == null) {
                return null;
            }
            AccessibilityNodeInfo node = root.findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
            return (node != null && node.isEditable()) ? node : null;
        } catch (Exception e) {
            Log.w(TAG, "find focused input failed", e);
            return null;
        }
    }

    /** 读当前文本与光标选区；越界/未知时退化为"末尾追加"。 */
    private static int clampSel(int value, int length) {
        return (value < 0 || value > length) ? length : value;
    }

    /** 节点身份：窗口 + 资源 id + 位置。切焦点后影子必须重新对齐。 */
    private static String nodeKeyOf(AccessibilityNodeInfo node) {
        Rect bounds = new Rect();
        node.getBoundsInScreen(bounds);
        return node.getWindowId() + "|" + node.getViewIdResourceName() + "|"
                + bounds.toShortString();
    }

    private void syncShadow(AccessibilityNodeInfo node) {
        final String key = nodeKeyOf(node);
        CharSequence cs = node.getText();
        String actual = cs == null ? "" : cs.toString();
        // 空输入框的 hint 会经 getText() 暴露出来（实测 SearchView 的「搜索…」）。
        // 若把它当成已有内容，用户第一次输入就会把提示文字一起写进输入框。
        CharSequence hintCs = node.getHintText();
        if (hintCs != null && hintCs.length() > 0 && actual.equals(hintCs.toString())) {
            actual = "";
        }
        final long now = SystemClock.uptimeMillis();

        if (shadowText == null || !key.equals(shadowKey)) {
            shadowKey = key;
            shadowText = actual;
            shadowSelStart = clampSel(node.getTextSelectionStart(), actual.length());
            shadowSelEnd = clampSel(node.getTextSelectionEnd(), actual.length());
            return;
        }
        if (actual.equals(shadowText)) {
            // 目标已追上影子：此时允许手机端光标/选区变化生效。
            shadowSelStart = clampSel(node.getTextSelectionStart(), actual.length());
            shadowSelEnd = clampSel(node.getTextSelectionEnd(), actual.length());
            return;
        }
        // 不一致：刚发过 SET_TEXT（<500ms）时大概率只是异步落地延迟，继续用影子，
        // 否则视为外部修改（清除按钮 / 手机键盘），重新以节点内容为准。
        if (now - shadowSentMs > 500) {
            shadowText = actual;
            shadowSelStart = clampSel(node.getTextSelectionStart(), actual.length());
            shadowSelEnd = clampSel(node.getTextSelectionEnd(), actual.length());
        }
    }

    private int insertText(AccessibilityNodeInfo node, String insert) {
        syncShadow(node);
        int start = clampSel(shadowSelStart, shadowText.length());
        int end = clampSel(shadowSelEnd, shadowText.length());
        if (start > end) {
            int tmp = start;
            start = end;
            end = tmp;
        }
        shadowText = shadowText.substring(0, start) + insert + shadowText.substring(end);
        shadowSelStart = shadowSelEnd = start + insert.length();
        shadowSentMs = SystemClock.uptimeMillis();
        return setNodeText(node, shadowText, shadowSelStart) ? 1 : -1;
    }

    private int deleteBackward(AccessibilityNodeInfo node) {
        syncShadow(node);
        int start = clampSel(shadowSelStart, shadowText.length());
        int end = clampSel(shadowSelEnd, shadowText.length());
        if (start > end) {
            int tmp = start;
            start = end;
            end = tmp;
        }
        if (start == end) {
            if (start == 0) {
                return 1; // 空输入框，无事可做
            }
            start--;
        }
        shadowText = shadowText.substring(0, start) + shadowText.substring(end);
        shadowSelStart = shadowSelEnd = start;
        shadowSentMs = SystemClock.uptimeMillis();
        return setNodeText(node, shadowText, start) ? 1 : -1;
    }

    private int pressEnter(AccessibilityNodeInfo node) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                // API 30+：由系统 IME 执行"回车"动作（搜索框会真正触发搜索）。
                if (node.performAction(
                        AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.getId())) {
                    return 1;
                }
            } catch (Exception e) {
                Log.w(TAG, "ACTION_IME_ENTER failed", e);
            }
        }
        // 多行输入框兜底：插入换行符。
        return insertText(node, "\n");
    }

    private static boolean setNodeText(AccessibilityNodeInfo node, String text, int caret) {
        try {
            int pos = Math.max(0, Math.min(caret, text.length()));
            Bundle args = new Bundle();
            args.putCharSequence(
                    AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, text);
            args.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, pos);
            args.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, pos);
            return node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args);
        } catch (Exception e) {
            Log.w(TAG, "ACTION_SET_TEXT failed", e);
            return false;
        }
    }

    /** 浏览器 e.code / e.keyCode → 文本。覆盖英文、数字、空格与常见标点。 */
    private static String charForCode(int keycode, String code, boolean shift) {
        if (code != null) {
            switch (code) {
                case "Space": return " ";
                case "Comma": return shift ? "<" : ",";
                case "Period": return shift ? ">" : ".";
                case "Slash": return shift ? "?" : "/";
                case "Semicolon": return shift ? ":" : ";";
                case "Quote": return shift ? "\"" : "'";
                case "Minus": return shift ? "_" : "-";
                case "Equal": return shift ? "+" : "=";
                case "BracketLeft": return shift ? "{" : "[";
                case "BracketRight": return shift ? "}" : "]";
                case "Backslash": return shift ? "|" : "\\";
                case "Backquote": return shift ? "~" : "`";
                default: break;
            }
            if (code.length() == 4 && code.startsWith("Key")) {
                char c = code.charAt(3);
                if (c >= 'A' && c <= 'Z') {
                    return String.valueOf(shift ? c : Character.toLowerCase(c));
                }
            }
            if (code.length() == 6 && code.startsWith("Digit")) {
                char d = code.charAt(5);
                if (d >= '0' && d <= '9') {
                    return String.valueOf(d);
                }
            }
            if (code.length() == 7 && code.startsWith("Numpad")) {
                char d = code.charAt(6);
                if (d >= '0' && d <= '9') {
                    return String.valueOf(d);
                }
            }
        }
        if (keycode == 32) return " ";
        if (keycode >= 48 && keycode <= 57) return String.valueOf((char) keycode);
        if (keycode >= 65 && keycode <= 90) {
            return String.valueOf(shift ? (char) keycode
                    : Character.toLowerCase((char) keycode));
        }
        switch (keycode) {
            case 186: return shift ? ":" : ";";
            case 187: return shift ? "+" : "=";
            case 188: return shift ? "<" : ",";
            case 189: return shift ? "_" : "-";
            case 190: return shift ? ">" : ".";
            case 191: return shift ? "?" : "/";
            case 192: return shift ? "~" : "`";
            case 219: return shift ? "{" : "[";
            case 220: return shift ? "|" : "\\";
            case 221: return shift ? "}" : "]";
            case 222: return shift ? "\"" : "'";
            default: return null;
        }
    }

    /** 当前物理屏幕宽度（含旋转）。服务未连接时返回 0，让上层回退到捕获帧尺寸。 */
    public static int screenWidth() {
        return screenSize(true);
    }

    /** 当前物理屏幕高度（含旋转）。服务未连接时返回 0。 */
    public static int screenHeight() {
        return screenSize(false);
    }

    private static int screenSize(boolean wantWidth) {
        final InputAccessibilityService service = sInstance;
        if (service == null) {
            return 0;
        }
        try {
            DisplayManager dm = (DisplayManager) service.getSystemService(Context.DISPLAY_SERVICE);
            if (dm == null) {
                return 0;
            }
            Display display = dm.getDisplay(Display.DEFAULT_DISPLAY);
            if (display == null) {
                return 0;
            }
            DisplayMetrics metrics = new DisplayMetrics();
            display.getRealMetrics(metrics);
            return wantWidth ? metrics.widthPixels : metrics.heightPixels;
        } catch (Exception e) {
            Log.w(TAG, "read real screen size failed", e);
            return 0;
        }
    }

    // ==================== 手势派发 ====================

    private boolean performStroke(int x1, int y1, int x2, int y2, int durationMs) {
        final Path path = new Path();
        path.moveTo(x1, y1);
        if (x1 != x2 || y1 != y2) {
            path.lineTo(x2, y2);
        }
        // 手势必须在主线程派发；post 保证调用线程（Qt 主线程）不阻塞。
        mainHandler.post(() -> {
            GestureDescription.Builder builder = new GestureDescription.Builder();
            builder.addStroke(new GestureDescription.StrokeDescription(path, 0, durationMs));
            dispatchGesture(builder.build(), null, null);
        });
        return true;
    }
}
