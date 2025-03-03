#include "tools/cabana/videowidget.h"

#include <algorithm>
#include <utility>

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedLayout>
#include <QStyleOptionSlider>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "tools/cabana/streams/replaystream.h"

const int MIN_VIDEO_HEIGHT = 100;
const int THUMBNAIL_MARGIN = 3;

static const QColor timeline_colors[] = {
  [(int)TimelineType::None] = QColor(111, 143, 175),
  [(int)TimelineType::Engaged] = QColor(0, 163, 108),
  [(int)TimelineType::UserFlag] = Qt::magenta,
  [(int)TimelineType::AlertInfo] = Qt::green,
  [(int)TimelineType::AlertWarning] = QColor(255, 195, 0),
  [(int)TimelineType::AlertCritical] = QColor(199, 0, 57),
};

VideoWidget::VideoWidget(QWidget *parent) : QFrame(parent) {
  setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
  auto main_layout = new QVBoxLayout(this);
  if (!can->liveStreaming())
    main_layout->addWidget(createCameraWidget());
  main_layout->addLayout(createPlaybackController());

  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  QObject::connect(can, &AbstractStream::paused, this, &VideoWidget::updatePlayBtnState);
  QObject::connect(can, &AbstractStream::resume, this, &VideoWidget::updatePlayBtnState);
  QObject::connect(can, &AbstractStream::msgsReceived, this, &VideoWidget::updateState);

  updatePlayBtnState();
  setWhatsThis(tr(R"(
    <b>Video</b><br />
    <!-- TODO: add descprition here -->
    <span style="color:gray">Timeline color</span>
    <table>
    <tr><td><span style="color:%1;">■ </span>Disengaged </td>
        <td><span style="color:%2;">■ </span>Engaged</td></tr>
    <tr><td><span style="color:%3;">■ </span>User Flag </td>
        <td><span style="color:%4;">■ </span>Info</td></tr>
    <tr><td><span style="color:%5;">■ </span>Warning </td>
        <td><span style="color:%6;">■ </span>Critical</td></tr>
    </table>
    <span style="color:gray">Shortcuts</span><br/>
    Pause/Resume: <span style="background-color:lightGray;color:gray">&nbsp;space&nbsp;</span>
  )").arg(timeline_colors[(int)TimelineType::None].name(),
          timeline_colors[(int)TimelineType::Engaged].name(),
          timeline_colors[(int)TimelineType::UserFlag].name(),
          timeline_colors[(int)TimelineType::AlertInfo].name(),
          timeline_colors[(int)TimelineType::AlertWarning].name(),
          timeline_colors[(int)TimelineType::AlertCritical].name()));
}

QHBoxLayout *VideoWidget::createPlaybackController() {
  QHBoxLayout *layout = new QHBoxLayout();
  layout->addWidget(seek_backward_btn = new ToolButton("rewind", tr("Seek backward")));
  layout->addWidget(play_btn = new ToolButton("play", tr("Play")));
  layout->addWidget(seek_forward_btn = new ToolButton("fast-forward", tr("Seek forward")));

  if (can->liveStreaming()) {
    layout->addWidget(skip_to_end_btn = new ToolButton("skip-end", tr("Skip to the end"), this));
    QObject::connect(skip_to_end_btn, &QToolButton::clicked, [this]() {
      // set speed to 1.0
      speed_btn->menu()->actions()[7]->setChecked(true);
      can->pause(false);
      can->seekTo(can->totalSeconds() + 1);
    });
  }

  layout->addWidget(time_btn = new QToolButton);
  time_btn->setToolTip(settings.absolute_time ? tr("Elapsed time") : tr("Absolute time"));
  time_btn->setAutoRaise(true);
  layout->addStretch(0);

  if (!can->liveStreaming()) {
    layout->addWidget(loop_btn = new ToolButton("repeat", tr("Loop playback")));
    QObject::connect(loop_btn, &QToolButton::clicked, this, &VideoWidget::loopPlaybackClicked);
  }

  // speed selector
  layout->addWidget(speed_btn = new QToolButton(this));
  speed_btn->setAutoRaise(true);
  speed_btn->setMenu(new QMenu(speed_btn));
  speed_btn->setPopupMode(QToolButton::InstantPopup);
  QActionGroup *speed_group = new QActionGroup(this);
  speed_group->setExclusive(true);

  int max_width = 0;
  QFont font = speed_btn->font();
  font.setBold(true);
  speed_btn->setFont(font);
  QFontMetrics fm(font);
  for (float speed : {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 0.8, 1., 2., 3., 5.}) {
    QString name = QString("%1x").arg(speed);
    max_width = std::max(max_width, fm.width(name) + fm.horizontalAdvance(QLatin1Char(' ')) * 2);

    QAction *act = new QAction(name, speed_group);
    act->setCheckable(true);
    QObject::connect(act, &QAction::toggled, [this, speed]() {
      can->setSpeed(speed);
      speed_btn->setText(QString("%1x  ").arg(speed));
    });
    speed_btn->menu()->addAction(act);
    if (speed == 1.0)act->setChecked(true);
  }
  speed_btn->setMinimumWidth(max_width + style()->pixelMetric(QStyle::PM_MenuButtonIndicator));

  QObject::connect(play_btn, &QToolButton::clicked, []() { can->pause(!can->isPaused()); });
  QObject::connect(seek_backward_btn, &QToolButton::clicked, []() { can->seekTo(can->currentSec() - 1); });
  QObject::connect(seek_forward_btn, &QToolButton::clicked, []() { can->seekTo(can->currentSec() + 1); });
  QObject::connect(time_btn, &QToolButton::clicked, [this]() {
    settings.absolute_time = !settings.absolute_time;
    time_btn->setToolTip(settings.absolute_time ? tr("Elapsed time") : tr("Absolute time"));
    updateState();
  });
  return layout;
}

QWidget *VideoWidget::createCameraWidget() {
  QWidget *w = new QWidget(this);
  QVBoxLayout *l = new QVBoxLayout(w);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(0);

  l->addWidget(camera_tab = new TabBar(w));
  camera_tab->setAutoHide(true);
  camera_tab->setExpanding(false);

  QStackedLayout *stacked = new QStackedLayout();
  stacked->setStackingMode(QStackedLayout::StackAll);
  stacked->addWidget(cam_widget = new CameraView("camerad", VISION_STREAM_ROAD, false));
  cam_widget->setMinimumHeight(MIN_VIDEO_HEIGHT);
  cam_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
  stacked->addWidget(alert_label = new InfoLabel(this));
  l->addLayout(stacked);

  l->addWidget(slider = new Slider(w));
  slider->setSingleStep(0);

  setMaximumTime(can->totalSeconds());
  QObject::connect(slider, &QSlider::sliderReleased, [this]() { can->seekTo(slider->currentSecond()); });
  QObject::connect(slider, &Slider::updateMaximumTime, this, &VideoWidget::setMaximumTime, Qt::QueuedConnection);
  QObject::connect(static_cast<ReplayStream*>(can), &ReplayStream::qLogLoaded, slider, &Slider::parseQLog);
  QObject::connect(cam_widget, &CameraWidget::clicked, []() { can->pause(!can->isPaused()); });
  QObject::connect(cam_widget, &CameraWidget::vipcAvailableStreamsUpdated, this, &VideoWidget::vipcAvailableStreamsUpdated);
  QObject::connect(camera_tab, &QTabBar::currentChanged, [this](int index) {
    if (index != -1) cam_widget->setStreamType((VisionStreamType)camera_tab->tabData(index).toInt());
  });
  return w;
}

void VideoWidget::vipcAvailableStreamsUpdated() {
  static const QString stream_names[] = {
    [VISION_STREAM_ROAD] = "Road camera",
    [VISION_STREAM_WIDE_ROAD] = "Wide road camera",
    [VISION_STREAM_DRIVER] = "Driver camera"};

  const auto &streams = cam_widget->availableStreams();
  for (int i = 0; i < streams.size(); ++i) {
    if (camera_tab->count() <= i) {
      camera_tab->addTab(QString());
    }
    int type = *std::next(streams.begin(), i);
    camera_tab->setTabText(i, stream_names[type]);
    camera_tab->setTabData(i, type);
  }
  while (camera_tab->count() > streams.size()) {
    camera_tab->removeTab(camera_tab->count() - 1);
  }
}

void VideoWidget::loopPlaybackClicked() {
  auto replay = qobject_cast<ReplayStream *>(can)->getReplay();
  if (!replay) return;

  if (replay->hasFlag(REPLAY_FLAG_NO_LOOP)) {
    replay->removeFlag(REPLAY_FLAG_NO_LOOP);
    loop_btn->setIcon("repeat");
  } else {
    replay->addFlag(REPLAY_FLAG_NO_LOOP);
    loop_btn->setIcon("repeat-1");
  }
}

void VideoWidget::setMaximumTime(double sec) {
  maximum_time = sec;
  slider->setTimeRange(0, sec);
}

void VideoWidget::updateTimeRange(double min, double max, bool is_zoomed) {
  if (can->liveStreaming()) {
    skip_to_end_btn->setEnabled(!is_zoomed);
    return;
  }
  is_zoomed ? slider->setTimeRange(min, max)
            : slider->setTimeRange(0, maximum_time);
}

QString VideoWidget::formatTime(double sec, bool include_milliseconds) {
  if (settings.absolute_time)
    sec = can->beginDateTime().addMSecs(sec * 1000).toMSecsSinceEpoch() / 1000.0;
  return utils::formatSeconds(sec, include_milliseconds, settings.absolute_time);
}

void VideoWidget::updateState() {
  if (slider) {
    if (!slider->isSliderDown())
      slider->setCurrentSecond(can->currentSec());
    alert_label->showAlert(slider->alertInfo(can->currentSec()));
    time_btn->setText(QString("%1 / %2").arg(formatTime(can->currentSec(), true),
                                             formatTime(slider->maximum() / slider->factor)));
  } else {
    time_btn->setText(formatTime(can->currentSec(), true));
  }
}

void VideoWidget::updatePlayBtnState() {
  play_btn->setIcon(can->isPaused() ? "play" : "pause");
  play_btn->setToolTip(can->isPaused() ? tr("Play") : tr("Pause"));
}

// Slider

Slider::Slider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
  thumbnail_label = new InfoLabel(parent);
  setMouseTracking(true);
}

AlertInfo Slider::alertInfo(double seconds) {
  uint64_t mono_time = (seconds + can->routeStartTime()) * 1e9;
  auto alert_it = alerts.lower_bound(mono_time);
  bool has_alert = (alert_it != alerts.end()) && ((alert_it->first - mono_time) <= 1e8);
  return has_alert ? alert_it->second : AlertInfo{};
}

QPixmap Slider::thumbnail(double seconds)  {
  uint64_t mono_time = (seconds + can->routeStartTime()) * 1e9;
  auto it = thumbnails.lowerBound(mono_time);
  return it != thumbnails.end() ? it.value() : QPixmap();
}

void Slider::setTimeRange(double min, double max) {
  assert(min < max);
  setRange(min * factor, max * factor);
}

void Slider::parseQLog(int segnum, std::shared_ptr<LogReader> qlog) {
 const auto &segments = qobject_cast<ReplayStream *>(can)->route()->segments();
  if (segments.size() > 0 && segnum == segments.rbegin()->first && !qlog->events.empty()) {
    emit updateMaximumTime(qlog->events.back()->mono_time / 1e9 - can->routeStartTime());
  }

  std::mutex mutex;
  QtConcurrent::blockingMap(qlog->events.cbegin(), qlog->events.cend(), [&mutex, this](const Event *e) {
    if (e->which == cereal::Event::Which::THUMBNAIL) {
      auto thumb = e->event.getThumbnail();
      auto data = thumb.getThumbnail();
      if (QPixmap pm; pm.loadFromData(data.begin(), data.size(), "jpeg")) {
        QPixmap scaled = pm.scaledToHeight(MIN_VIDEO_HEIGHT - THUMBNAIL_MARGIN * 2, Qt::SmoothTransformation);
        std::lock_guard lk(mutex);
        thumbnails[thumb.getTimestampEof()] = scaled;
      }
    } else if (e->which == cereal::Event::Which::CONTROLS_STATE) {
      auto cs = e->event.getControlsState();
      if (cs.getAlertType().size() > 0 && cs.getAlertText1().size() > 0 &&
          cs.getAlertSize() != cereal::ControlsState::AlertSize::NONE) {
        std::lock_guard lk(mutex);
        alerts.emplace(e->mono_time, AlertInfo{cs.getAlertStatus(), cs.getAlertText1().cStr(), cs.getAlertText2().cStr()});
      }
    }
  });
  update();
}

void Slider::paintEvent(QPaintEvent *ev) {
  QPainter p(this);
  QRect r = rect().adjusted(0, 4, 0, -4);
  p.fillRect(r, timeline_colors[(int)TimelineType::None]);
  double min = minimum() / factor;
  double max = maximum() / factor;

  auto fillRange = [&](double begin, double end, const QColor &color) {
    if (begin > max || end < min) return;
    r.setLeft(((std::max(min, begin) - min) / (max - min)) * width());
    r.setRight(((std::min(max, end) - min) / (max - min)) * width());
    p.fillRect(r, color);
  };

  const auto replay = qobject_cast<ReplayStream *>(can)->getReplay();
  for (auto [begin, end, type] : replay->getTimeline()) {
    fillRange(begin, end, timeline_colors[(int)type]);
  }

  QColor empty_color = palette().color(QPalette::Window);
  empty_color.setAlpha(160);
  for (const auto &[n, seg] : replay->segments()) {
    if (!(seg && seg->isLoaded()))
      fillRange(n * 60.0, (n + 1) * 60.0, empty_color);
  }

  QStyleOptionSlider opt;
  opt.initFrom(this);
  opt.minimum = minimum();
  opt.maximum = maximum();
  opt.subControls = QStyle::SC_SliderHandle;
  opt.sliderPosition = value();
  style()->drawComplexControl(QStyle::CC_Slider, &opt, &p);
}

void Slider::mousePressEvent(QMouseEvent *e) {
  QSlider::mousePressEvent(e);
  if (e->button() == Qt::LeftButton && !isSliderDown()) {
    setValue(minimum() + ((maximum() - minimum()) * e->x()) / width());
    emit sliderReleased();
  }
}

void Slider::mouseMoveEvent(QMouseEvent *e) {
  int pos = std::clamp(e->pos().x(), 0, width());
  double seconds = (minimum() + pos * ((maximum() - minimum()) / (double)width())) / factor;
  QPixmap thumb = thumbnail(seconds);
  if (!thumb.isNull()) {
    int x = std::clamp(pos - thumb.width() / 2, THUMBNAIL_MARGIN, width() - thumb.width() - THUMBNAIL_MARGIN + 1);
    int y = -thumb.height() - THUMBNAIL_MARGIN;
    thumbnail_label->showPixmap(mapToParent(QPoint(x, y)), utils::formatSeconds(seconds), thumb, alertInfo(seconds));
  } else {
    thumbnail_label->hide();
  }
  QSlider::mouseMoveEvent(e);
}

bool Slider::event(QEvent *event) {
  switch (event->type()) {
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::Leave:
      thumbnail_label->hide();
      break;
    default:
      break;
  }
  return QSlider::event(event);
}

// InfoLabel

InfoLabel::InfoLabel(QWidget *parent) : QWidget(parent, Qt::WindowStaysOnTopHint) {
  setAttribute(Qt::WA_ShowWithoutActivating);
  setVisible(false);
}

void InfoLabel::showPixmap(const QPoint &pt, const QString &sec, const QPixmap &pm, const AlertInfo &alert) {
  second = sec;
  pixmap = pm;
  alert_info = alert;
  setGeometry(QRect(pt, pm.size()));
  setVisible(true);
  update();
}

void InfoLabel::showAlert(const AlertInfo &alert) {
  alert_info = alert;
  pixmap = {};
  setVisible(!alert_info.text1.isEmpty());
  update();
}

void InfoLabel::paintEvent(QPaintEvent *event) {
  QPainter p(this);
  p.setPen(QPen(palette().color(QPalette::BrightText), 2));
  if (!pixmap.isNull()) {
    p.drawPixmap(0, 0, pixmap);
    p.drawRect(rect());
    p.drawText(rect().adjusted(0, 0, 0, -THUMBNAIL_MARGIN), second, Qt::AlignHCenter | Qt::AlignBottom);
  }
  if (alert_info.text1.size() > 0) {
    QColor color = timeline_colors[(int)TimelineType::AlertInfo];
    if (alert_info.status == cereal::ControlsState::AlertStatus::USER_PROMPT) {
      color = timeline_colors[(int)TimelineType::AlertWarning];
    } else if (alert_info.status == cereal::ControlsState::AlertStatus::CRITICAL) {
      color = timeline_colors[(int)TimelineType::AlertCritical];
    }
    color.setAlphaF(0.5);
    QString text = alert_info.text1;
    if (!alert_info.text2.isEmpty()) {
      text += "\n" + alert_info.text2;
    }

    if (!pixmap.isNull()) {
      QFont font;
      font.setPixelSize(11);
      p.setFont(font);
    }
    QRect text_rect = rect().adjusted(2, 2, -2, -2);
    QRect r = p.fontMetrics().boundingRect(text_rect, Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, text);
    p.fillRect(text_rect.left(), r.top(), text_rect.width(), r.height(), color);
    p.drawText(text_rect, Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, text);
  }
}
