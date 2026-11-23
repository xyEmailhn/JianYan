package ai.jianyan;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * 简言 Playground 的唯一一个 Activity。
 *
 * 界面全在 assets/index.html 里（WebView 画），编译与执行在 native 层。
 * 分工的理由：编辑器这种东西用 HTML 写一天就够，用原生控件写要一周，
 * 而且中文输入法、选中、撤销这些细节 WebView 已经替我们处理好了。
 */
public class MainActivity extends Activity {

    static {
        System.loadLibrary("jianyan");
    }

    /** native 实现：编译并执行一段简言源码，返回全部输出。 */
    public static native String run(String 源码, String 工作目录);

    private String 工作目录;
    private WebView 网页;

    @Override
    protected void onCreate(Bundle 状态) {
        super.onCreate(状态);
        工作目录 = getFilesDir().getAbsolutePath();

        网页 = new WebView(this);
        WebSettings 设 = 网页.getSettings();
        设.setJavaScriptEnabled(true);
        设.setDomStorageEnabled(true);
        设.setAllowFileAccess(true);
        设.setSupportZoom(false);
        设.setBuiltInZoomControls(false);
        // 让编辑器自己处理软键盘，别让 WebView 抢焦点
        网页.setFocusable(true);
        网页.setFocusableInTouchMode(true);
        网页.setWebViewClient(new WebViewClient());
        网页.addJavascriptInterface(new 桥(), "简言");
        网页.loadUrl("file:///android_asset/index.html");

        setContentView(网页);
    }

    /** 暴露给 JS 的对象。方法名带 @JavascriptInterface 才会被注入。 */
    public class 桥 {
        @JavascriptInterface
        public String 运行(String 源码) {
            try {
                return run(源码, 工作目录);
            } catch (Throwable e) {
                return "执行器出错：" + e.toString() + "\n";
            }
        }

        @JavascriptInterface
        public String 版本() {
            return "0.17";
        }
    }

    @Override
    public void onBackPressed() {
        if (网页 != null && 网页.canGoBack()) 网页.goBack();
        else super.onBackPressed();
    }
}
