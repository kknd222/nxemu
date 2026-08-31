package org.nxemu.app

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlin.math.roundToInt

class HomeMenuActivity : Activity() {
    private lateinit var gamePath: String
    private lateinit var gameName: String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE

        gamePath = intent.getStringExtra(PerGameSettingsActivity.EXTRA_GAME_PATH).orEmpty()
            .ifBlank { AppPreferences.lastGamePath(this) }
        gameName = intent.getStringExtra(PerGameSettingsActivity.EXTRA_GAME_NAME).orEmpty()
            .ifBlank { AppPreferences.lastGameName(this) }
            .ifBlank { deriveName(gamePath) }
        setContentView(buildContent())
    }

    private fun buildContent(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(26), dp(16), dp(26), dp(16))
            background = backgroundGradient()
        }

        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(this@HomeMenuActivity).apply {
                text = "NxEmu"
                textSize = 26f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xff000000.toInt())
            })
            addView(TextView(this@HomeMenuActivity).apply {
                text = "配置中心"
                textSize = 13f
                setTextColor(0xff45464f.toInt())
            })
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        header.addView(pillButton("返回") { finish() })
        root.addView(header)

        root.addView(TextView(this).apply {
            text = if (gamePath.isBlank()) {
                "当前游戏：未选择 · 可先扫描或授权目录 · 原始NSP/XCI=${rawContainerLabel()}"
            } else {
                "当前游戏：${gameName.ifBlank { deriveName(gamePath) }} · 原始NSP/XCI=${rawContainerLabel()}"
            }
            textSize = 13f
            maxLines = 1
            setTextColor(0xff45464f.toInt())
            setPadding(0, dp(7), 0, dp(9))
        })

        val grid = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.TOP
        }
        grid.addView(column(
            section("文件与游戏库",
                item("选择 DXCI/DNSP/NRO", "通过系统文件选择器导入单个镜像。") {
                    homeAction(MainActivity.HOME_ACTION_OPEN_FILE)
                },
                item("授权 ROM 文件夹", "选择外置目录；授权后下次启动自动扫描。") {
                    homeAction(MainActivity.HOME_ACTION_OPEN_FOLDER)
                },
                item("扫描 /sdcard/ns/rom", "重新扫描默认 ROM 目录并去重。") {
                    homeAction(MainActivity.HOME_ACTION_SCAN_DEFAULT)
                },
                item("原始 NSP/XCI：${rawContainerLabel()}", "默认隐藏原始容器；只保留 DNSP/DXCI/NRO，避免误启动未转换文件。") {
                    AppPreferences.saveShowRawSwitchContainers(this, !AppPreferences.showRawSwitchContainers(this))
                    setContentView(buildContent())
                },
                item("运行上次游戏", "使用最后一次保存的游戏路径启动。") {
                    homeAction(MainActivity.HOME_ACTION_RUN_LAST)
                }
            ),
            section("当前游戏",
                item("属性 / 独立配置", "分辨率、跳帧、NCE、驱动、日志等每游戏覆盖项。") {
                    openPerGameSettings()
                },
                item("信息 / 存档 / Add-on", "进入游戏管理页，查看信息、存档和附加内容入口。") {
                    openGameManagement()
                },
                item("刷新封面缓存", "重新匹配 /sdcard/ns/covers 下的封面。") {
                    homeAction(MainActivity.HOME_ACTION_REFRESH_COVERS)
                }
            )
        ))
        grid.addView(column(
            section("系统设置",
                item("全局设置", "全局分辨率、性能 HUD、日志、兼容选项默认值。") {
                    startActivity(Intent(this, SettingsActivity::class.java))
                },
                item("输入 / 触控设置", "触控布局、摇杆/按键大小、透明度和手柄相关入口。") {
                    startActivity(Intent(this, InputSettingsActivity::class.java))
                },
                item("GPU 驱动管理", "选择/安装 Turnip/Adreno 驱动，查看当前驱动状态。") {
                    startActivity(Intent(this, DriverManagerActivity::class.java))
                },
                item("文件访问权限", "跳转系统授权页；Android 不允许静默自动授权。") {
                    homeAction(MainActivity.HOME_ACTION_ALL_FILES)
                }
            ),
            section("诊断与维护",
                item("复制首页日志", "复制当前首页诊断信息，方便反馈问题。") {
                    homeAction(MainActivity.HOME_ACTION_COPY_LOG)
                },
                item("返回游戏库", "关闭配置中心，回到首页。") {
                    finish()
                }
            )
        ))
        root.addView(grid, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        return ScrollView(this).apply {
            isFillViewport = true
            addView(root)
        }
    }

    private fun column(vararg sections: View): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        sections.forEach { addView(it) }
        layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
            setMargins(dp(6), 0, dp(6), 0)
        }
    }

    private fun section(title: String, vararg items: View): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(9), dp(12), dp(9))
            background = panelBackground()
            addView(LinearLayout(this@HomeMenuActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, 0, 0, dp(6))
                addView(TextView(this@HomeMenuActivity).apply {
                    text = sectionGlyph(title)
                    textSize = 12f
                    gravity = Gravity.CENTER
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(0xffffffff.toInt())
                    background = miniBadgeBackground()
                }, LinearLayout.LayoutParams(dp(26), dp(26)))
                addView(TextView(this@HomeMenuActivity).apply {
                    text = title
                    textSize = 16f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(0xff000000.toInt())
                    setPadding(dp(8), 0, 0, 0)
                }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            })
            items.forEach { addView(it) }
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(6), 0, dp(9))
            }
        }
    }

    private fun item(title: String, subtitle: String, action: () -> Unit): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            isClickable = true
            isFocusable = true
            setPadding(dp(10), dp(7), dp(9), dp(7))
            background = itemBackground()
            setOnClickListener { action() }
            addView(TextView(this@HomeMenuActivity).apply {
                text = itemGlyph(title)
                textSize = 13f
                gravity = Gravity.CENTER
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xff4c4fbe.toInt())
                background = smallIconBackground()
            }, LinearLayout.LayoutParams(dp(32), dp(32)))
            addView(LinearLayout(this@HomeMenuActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(10), 0, dp(6), 0)
                addView(TextView(this@HomeMenuActivity).apply {
                    text = title
                    textSize = 14.5f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(0xff000000.toInt())
                    maxLines = 1
                })
                addView(TextView(this@HomeMenuActivity).apply {
                    text = subtitle
                    textSize = 11f
                    maxLines = 1
                    setTextColor(0xff45464f.toInt())
                    setPadding(0, dp(2), 0, 0)
                })
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(TextView(this@HomeMenuActivity).apply {
                text = if (title.contains("原始 NSP/XCI")) rawContainerLabel() else "›"
                textSize = if (title.contains("原始 NSP/XCI")) 11f else 20f
                gravity = Gravity.CENTER
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xff4c4fbe.toInt())
                background = trailingPillBackground()
            }, LinearLayout.LayoutParams(if (title.contains("原始 NSP/XCI")) dp(48) else dp(28), dp(28)))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(3), 0, dp(3))
            }
        }
    }

    private fun sectionGlyph(title: String): String = when {
        title.contains("文件") -> "F"
        title.contains("当前") -> "G"
        title.contains("系统") -> "S"
        title.contains("诊断") -> "D"
        else -> "N"
    }

    private fun itemGlyph(title: String): String = when {
        title.contains("选择") -> "+"
        title.contains("授权") || title.contains("权限") -> "A"
        title.contains("扫描") -> "R"
        title.contains("原始") -> "NS"
        title.contains("运行") || title.contains("启动") -> "▶"
        title.contains("属性") -> "P"
        title.contains("信息") || title.contains("存档") -> "I"
        title.contains("封面") -> "C"
        title.contains("全局") -> "⚙"
        title.contains("输入") -> "⌁"
        title.contains("GPU") || title.contains("驱动") -> "V"
        title.contains("复制") -> "L"
        title.contains("返回") -> "←"
        else -> "•"
    }

    private fun pillButton(label: String, action: () -> Unit): Button = Button(this).apply {
        text = label
        isAllCaps = false
        textSize = 14f
        setTextColor(0xff45464f.toInt())
        background = GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(24).toFloat()
            setColor(0x00ffffff)
            setStroke(dp(1), 0xff7c757f.toInt())
        }
        setOnClickListener { action() }
    }

    private fun homeAction(action: String) {
        startActivity(Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            putExtra(MainActivity.EXTRA_HOME_ACTION, action)
        })
        finish()
    }

    private fun openPerGameSettings() {
        if (gamePath.isBlank()) {
            Toast.makeText(this, "还没有选择游戏", Toast.LENGTH_SHORT).show()
            return
        }
        startActivity(Intent(this, PerGameSettingsActivity::class.java).apply {
            putExtra(PerGameSettingsActivity.EXTRA_GAME_PATH, gamePath)
            putExtra(PerGameSettingsActivity.EXTRA_GAME_NAME, gameName)
        })
    }

    private fun openGameManagement() {
        if (gamePath.isBlank()) {
            Toast.makeText(this, "还没有选择游戏", Toast.LENGTH_SHORT).show()
            return
        }
        startActivity(Intent(this, GameManagementActivity::class.java).apply {
            putExtra(GameManagementActivity.EXTRA_MODE, GameManagementActivity.MODE_INFO)
            putExtra(GameManagementActivity.EXTRA_GAME_PATH, gamePath)
            putExtra(GameManagementActivity.EXTRA_GAME_NAME, gameName)
        })
    }

    private fun deriveName(path: String): String = when {
        path.isBlank() -> ""
        path.startsWith("/") -> path.substringAfterLast('/')
        path.startsWith("content://") -> path.substringAfterLast('/').substringAfterLast(':')
        else -> path
    }

    private fun rawContainerLabel(): String = if (AppPreferences.showRawSwitchContainers(this)) "显示" else "隐藏"

    private fun backgroundGradient(): GradientDrawable = GradientDrawable(
        GradientDrawable.Orientation.TL_BR,
        intArrayOf(0xffffffff.toInt(), 0xfffffbff.toInt(), 0xfffbf7ff.toInt())
    )

    private fun panelBackground(): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(24).toFloat()
        setColor(0x7fffffff)
        setStroke(dp(1), 0xff7c757f.toInt())
    }

    private fun itemBackground(): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(18).toFloat()
        setColor(0xfff0f0f0.toInt())
        setStroke(dp(1), 0x667c757f)
    }

    private fun miniBadgeBackground(): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(9).toFloat()
        setColor(0xff4c4fbe.toInt())
    }

    private fun smallIconBackground(): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(12).toFloat()
        setColor(0x1a4c4fbe)
        setStroke(dp(1), 0x334c4fbe)
    }

    private fun trailingPillBackground(): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(14).toFloat()
        setColor(0x114c4fbe)
        setStroke(dp(1), 0x224c4fbe)
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).roundToInt()
}
