package org.libsdl.app;

import android.content.*;
import android.text.Editable;
import android.text.Selection;
import android.view.*;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

public class SDLInputConnection extends BaseInputConnection
{
    private static final int EDIT_KEY_BACKSPACE = 0;
    private static final int EDIT_KEY_DELETE = 1;
    private static final int EDIT_KEY_ENTER = 2;
    private static final int EDIT_KEY_LEFT = 3;
    private static final int EDIT_KEY_RIGHT = 4;
    private static final int EDIT_KEY_UP = 5;
    private static final int EDIT_KEY_DOWN = 6;
    private static final int EDIT_KEY_HOME = 7;
    private static final int EDIT_KEY_END = 8;
    private static final int NAVIGATION_PADDING = 512;
    private static final char NAVIGATION_PADDING_CHAR = 'x';
    private static final String NAVIGATION_PADDING_TEXT = buildNavigationPadding();

    protected View mTargetView;
    protected EditText mEditText;
    protected String mCommittedText = "";
    // Tracks the IME's requested cursor, which may lag behind our recentered hidden selection.
    protected int mNavigationSelectionStart = -1;
    protected int mNavigationSelectionEnd = -1;

    private static int editKeyFromKeyCode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_DEL:
                return EDIT_KEY_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL:
                return EDIT_KEY_DELETE;
            case KeyEvent.KEYCODE_ENTER:
                return EDIT_KEY_ENTER;
            case KeyEvent.KEYCODE_DPAD_LEFT:
            case KeyEvent.KEYCODE_SYSTEM_NAVIGATION_LEFT:
                return EDIT_KEY_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
            case KeyEvent.KEYCODE_SYSTEM_NAVIGATION_RIGHT:
                return EDIT_KEY_RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP:
            case KeyEvent.KEYCODE_SYSTEM_NAVIGATION_UP:
                return EDIT_KEY_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:
            case KeyEvent.KEYCODE_SYSTEM_NAVIGATION_DOWN:
                return EDIT_KEY_DOWN;
            case KeyEvent.KEYCODE_MOVE_HOME:
                return EDIT_KEY_HOME;
            case KeyEvent.KEYCODE_MOVE_END:
                return EDIT_KEY_END;
            default:
                return -1;
        }
    }

    public static boolean isEditingKeyCode(int keyCode) {
        return editKeyFromKeyCode(keyCode) >= 0;
    }

    private static String buildNavigationPadding() {
        StringBuilder padding = new StringBuilder(NAVIGATION_PADDING);
        for (int i = 0; i < NAVIGATION_PADDING; ++i) {
            padding.append(NAVIGATION_PADDING_CHAR);
        }
        return padding.toString();
    }

    public static boolean handleEditingKeyEvent(KeyEvent event) {
        int editKey = editKeyFromKeyCode(event.getKeyCode());
        if (editKey < 0) {
            return false;
        }

        if (event.getAction() == KeyEvent.ACTION_DOWN)
            return nativeTapEditingKey(editKey);
        return event.getAction() == KeyEvent.ACTION_UP && nativeIsEditingKeyTarget();
    }

    public SDLInputConnection(View targetView, boolean fullEditor) {
        super(targetView, fullEditor);
        mTargetView = targetView;
        mEditText = new EditText(SDL.getContext());
        ensureNavigationBuffer();
    }

    @Override
    public Editable getEditable() {
        Editable content = mEditText.getEditableText();
        ensureNavigationBuffer(content);
        return content;
    }

    protected void ensureNavigationBuffer() {
        ensureNavigationBuffer(mEditText.getEditableText());
    }

    protected void ensureNavigationBuffer(Editable content) {
        if (content == null) {
            return;
        }

        if (!hasNavigationPadding(content)) {
            replaceEditableWithApplicationText(content, applicationText(content));
        }

        int selectionStart = Selection.getSelectionStart(content);
        int selectionEnd = Selection.getSelectionEnd(content);
        if (selectionStart < NAVIGATION_PADDING / 2
                || selectionEnd < NAVIGATION_PADDING / 2
                || content.length() - selectionStart < NAVIGATION_PADDING / 2
                || content.length() - selectionEnd < NAVIGATION_PADDING / 2) {
            recenterNavigationSelection(content);
        }
    }

    protected String applicationText(Editable content) {
        if (content == null) {
            return "";
        }
        if (hasNavigationPadding(content)) {
            return content.subSequence(NAVIGATION_PADDING, content.length() - NAVIGATION_PADDING).toString();
        }
        return content.toString();
    }

    protected boolean hasNavigationPadding(Editable content) {
        if (content == null || content.length() < NAVIGATION_PADDING * 2) {
            return false;
        }
        for (int i = 0; i < NAVIGATION_PADDING; ++i) {
            if (content.charAt(i) != NAVIGATION_PADDING_CHAR
                    || content.charAt(content.length() - 1 - i) != NAVIGATION_PADDING_CHAR) {
                return false;
            }
        }
        return true;
    }

    protected void replaceEditableWithApplicationText(Editable content, String text) {
        content.replace(0, content.length(), NAVIGATION_PADDING_TEXT + text + NAVIGATION_PADDING_TEXT);
        recenterNavigationSelection(content);
    }

    protected int navigationAnchor(Editable content) {
        return Math.max(NAVIGATION_PADDING, content.length() - NAVIGATION_PADDING);
    }

    protected void setNavigationSelection(int start, int end) {
        mNavigationSelectionStart = start;
        mNavigationSelectionEnd = end;
    }

    protected int trackedSelectionStart(Editable content) {
        if (mNavigationSelectionStart >= 0) {
            return Math.min(content.length(), Math.max(0, mNavigationSelectionStart));
        }
        return Math.min(content.length(), Math.max(0, Selection.getSelectionStart(content)));
    }

    protected int trackedSelectionEnd(Editable content) {
        if (mNavigationSelectionEnd >= 0) {
            return Math.min(content.length(), Math.max(0, mNavigationSelectionEnd));
        }
        return Math.min(content.length(), Math.max(0, Selection.getSelectionEnd(content)));
    }

    protected void recenterNavigationSelection(Editable content) {
        recenterNavigationSelection(content, true, true);
    }

    protected void recenterNavigationSelection(Editable content, boolean updateTrackedSelection, boolean notifyIme) {
        int oldStart = Selection.getSelectionStart(content);
        int oldEnd = Selection.getSelectionEnd(content);
        int anchor = navigationAnchor(content);
        Selection.setSelection(content, anchor, anchor);
        if (updateTrackedSelection) {
            setNavigationSelection(anchor, anchor);
        }
        if (notifyIme) {
            notifySelectionChanged(oldStart, oldEnd, anchor, anchor);
        }
    }

    public int selectionAnchor() {
        ensureNavigationBuffer();
        return navigationAnchor(mEditText.getEditableText());
    }

    public String navigationBufferText() {
        ensureNavigationBuffer();
        return mEditText.getEditableText().toString();
    }

    protected void notifySelectionChanged(int oldStart, int oldEnd, int newStart, int newEnd) {
        if (mTargetView == null) {
            return;
        }

        InputMethodManager imm = (InputMethodManager)SDL.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.updateSelection(mTargetView, oldStart, oldEnd, newStart, newEnd);
        }
    }

    @Override
    public CharSequence getTextBeforeCursor(int length, int flags) {
        Editable content = mEditText.getEditableText();
        ensureNavigationBuffer(content);
        int end = trackedSelectionStart(content);
        int start = Math.max(0, end - Math.max(0, length));
        return content.subSequence(start, end);
    }

    @Override
    public CharSequence getTextAfterCursor(int length, int flags) {
        Editable content = mEditText.getEditableText();
        ensureNavigationBuffer(content);
        int start = trackedSelectionEnd(content);
        int end = Math.min(content.length(), start + Math.max(0, length));
        return content.subSequence(start, end);
    }

    @Override
    public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) {
        Editable content = mEditText.getEditableText();
        ensureNavigationBuffer(content);

        ExtractedText extracted = new ExtractedText();
        extracted.text = content.toString();
        extracted.startOffset = 0;
        extracted.partialStartOffset = -1;
        extracted.partialEndOffset = -1;
        extracted.selectionStart = trackedSelectionStart(content);
        extracted.selectionEnd = trackedSelectionEnd(content);
        extracted.flags = 0;
        return extracted;
    }

    @Override
    public boolean setSelection(int start, int end) {
        Editable content = mEditText.getEditableText();
        ensureNavigationBuffer(content);
        int contentLength = content.length();
        int safeStart = Math.min(contentLength - 1, Math.max(1, start));
        int safeEnd = Math.min(contentLength - 1, Math.max(1, end));
        int oldStart = mNavigationSelectionStart >= 0 ? mNavigationSelectionStart : Selection.getSelectionStart(content);
        int oldEnd = mNavigationSelectionEnd >= 0 ? mNavigationSelectionEnd : Selection.getSelectionEnd(content);
        boolean active = nativeIsEditingKeyTarget();
        if (active) {
            handleSelectionMovement(oldStart, oldEnd, safeStart, safeEnd, content);
            setNavigationSelection(safeStart, safeEnd);
            recenterNavigationSelection(content, false, false);
            notifySelectionChanged(oldStart, oldEnd, safeStart, safeEnd);
            return true;
        }
        boolean changed = super.setSelection(safeStart, safeEnd);
        if (changed) {
            setNavigationSelection(safeStart, safeEnd);
        }
        return changed;
    }

    protected boolean handleSelectionMovement(int oldStart, int oldEnd, int newStart, int newEnd, Editable content) {
        if (oldStart < 0 || oldEnd < 0 || oldStart != oldEnd || newStart != newEnd) {
            return false;
        }

        int editKey = -1;
        int contentLength = content.length();
        int anchor = navigationAnchor(content);
        if (newStart <= 1) {
            editKey = EDIT_KEY_HOME;
        } else if (newStart >= contentLength - 1) {
            editKey = EDIT_KEY_END;
        } else if (newStart < oldStart) {
            editKey = EDIT_KEY_LEFT;
        } else if (newStart > oldStart) {
            editKey = EDIT_KEY_RIGHT;
        } else if (newStart < anchor) {
            editKey = EDIT_KEY_LEFT;
        } else if (newStart > anchor) {
            editKey = EDIT_KEY_RIGHT;
        }

        if (editKey < 0) {
            return false;
        }

        return nativeTapEditingKey(editKey);
    }

    @Override
    public boolean sendKeyEvent(KeyEvent event) {
        /*
         * This used to handle the keycodes from soft keyboard (and IME-translated input from hardkeyboard)
         * However, as of Ice Cream Sandwich and later, almost all soft keyboard doesn't generate key presses
         * and so we need to generate them ourselves in commitText.  To avoid duplicates on the handful of keys
         * that still do, we empty this out.
         */

        /*
         * Return DOES still generate a key event, however.  So rather than using it as the 'click a button' key
         * as we do with physical keyboards, let's just use it to hide the keyboard.
         */

        if (event.getKeyCode() == KeyEvent.KEYCODE_ENTER) {
            boolean sent = false;
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                sent = nativeTapEditingKey(EDIT_KEY_ENTER);
            }
            if (sent || event.getAction() == KeyEvent.ACTION_UP) {
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN && SDLActivity.onNativeSoftReturnKey()) {
                return true;
            }
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_DEL) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                sendBackspace(1);
            }
            return true;
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_FORWARD_DEL) {
            boolean sent = false;
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                sent = nativeTapEditingKey(EDIT_KEY_DELETE);
            }
            if (sent || event.getAction() == KeyEvent.ACTION_UP) {
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_FORWARD_DEL);
                SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_FORWARD_DEL);
            }
            return true;
        }
        if (isEditingKeyCode(event.getKeyCode())) {
            if (handleEditingKeyEvent(event)) {
                return true;
            }
        }

        return super.sendKeyEvent(event);
    }

    @Override
    public boolean commitText(CharSequence text, int newCursorPosition) {
        ensureNavigationBuffer();
        if (!super.commitText(text, newCursorPosition)) {
            return false;
        }
        updateText();
        return true;
    }

    @Override
    public boolean setComposingText(CharSequence text, int newCursorPosition) {
        ensureNavigationBuffer();
        if (!super.setComposingText(text, newCursorPosition)) {
            return false;
        }
        updateText();
        return true;
    }

    @Override
    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
        ensureNavigationBuffer();
        if (beforeLength > 0 && afterLength == 0) {
            sendBackspace(beforeLength);
            return true;
        }
        if (beforeLength == 0 && afterLength > 0) {
            while (afterLength-- > 0) {
                if (!nativeTapEditingKey(EDIT_KEY_DELETE)) {
                    SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_FORWARD_DEL);
                    SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_FORWARD_DEL);
                }
            }
            return true;
        }

        if (!super.deleteSurroundingText(beforeLength, afterLength)) {
            return false;
        }
        updateText();
        return true;
    }

    protected void sendBackspace(int count) {
        trimCommittedText(count);
        while (count-- > 0) {
            if (!nativeTapEditingKey(EDIT_KEY_BACKSPACE)) {
                nativeGenerateScancodeForUnichar('\b');
            }
        }
    }

    protected void trimCommittedText(int count) {
        int end = mCommittedText.length();
        for (int i = 0; i < count && end > 0; ++i) {
            end = mCommittedText.offsetByCodePoints(end, -1);
        }
        mCommittedText = mCommittedText.substring(0, end);

        final Editable content = mEditText.getEditableText();
        if (content != null) {
            replaceEditableWithApplicationText(content, mCommittedText);
        }
    }

    @Override
    public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
        return deleteSurroundingText(beforeLength, afterLength);
    }

    protected void updateText() {
        final Editable content = getEditable();
        if (content == null) {
            return;
        }

        String text = applicationText(content);
        int compareLength = Math.min(text.length(), mCommittedText.length());
        int matchLength, offset;

        /* Backspace over characters that are no longer in the string */
        for (matchLength = 0; matchLength < compareLength; ) {
            int codePoint = mCommittedText.codePointAt(matchLength);
            if (codePoint != text.codePointAt(matchLength)) {
                break;
            }
            matchLength += Character.charCount(codePoint);
        }

        /* FIXME: This doesn't handle graphemes, like '🌬️' */
        for (offset = matchLength; offset < mCommittedText.length(); ) {
            int codePoint = mCommittedText.codePointAt(offset);
            if (!nativeTapEditingKey(EDIT_KEY_BACKSPACE)) {
                nativeGenerateScancodeForUnichar('\b');
            }
            offset += Character.charCount(codePoint);
        }

        if (matchLength < text.length()) {
            String pendingText = text.subSequence(matchLength, text.length()).toString();
            if (!SDLActivity.dispatchingKeyEvent()) {
                for (offset = 0; offset < pendingText.length(); ) {
                    int codePoint = pendingText.codePointAt(offset);
                    if (codePoint == '\n') {
                        if (SDLActivity.onNativeSoftReturnKey()) {
                            return;
                        }
                    }
                    /* Higher code points don't generate simulated scancodes */
                    if (codePoint > 0 && codePoint < 128) {
                        nativeGenerateScancodeForUnichar((char)codePoint);
                    }
                    offset += Character.charCount(codePoint);
                }
            }
            SDLInputConnection.nativeCommitText(pendingText, 0);
        }
        mCommittedText = text;
        ensureNavigationBuffer(content);
    }

    public static native void nativeCommitText(String text, int newCursorPosition);

    public static native void nativeGenerateScancodeForUnichar(char c);

    public static native boolean nativeTapEditingKey(int key);

    public static native boolean nativeIsEditingKeyTarget();
}
