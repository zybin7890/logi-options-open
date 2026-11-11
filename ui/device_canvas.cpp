#include "device_canvas.hpp"
#include "assets.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <algorithm>
#include <array>
#include <span>

namespace loo_ui {
namespace {
const QColor background("#1a1a1a"), surface("#151515"), accent("#00d8c6");
struct Point { double x, y; int align, width; const char* caption; };
constexpr std::array buttonPoints{
    Point{.71,.15,1,86,"滚轮按钮"}, Point{.81,.34,1,86,"顶部按钮"},
    Point{.55,.51,1,132,"拇指滚轮"}, Point{.35,.43,-1,86,"前进按钮"},
    Point{.45,.60,0,86,"后退按钮"}, Point{.08,.58,-1,92,"拇指按钮"}};
constexpr std::array pointerPoints{
    Point{.73,.16,1,148,"滚轮"}, Point{.83,.54,1,135,"指针速度"},
    Point{.55,.51,-1,132,"拇指滚轮"}};
std::span<const Point> points(DeviceMode mode) {
    return mode==DeviceMode::Buttons?std::span<const Point>(buttonPoints):std::span<const Point>(pointerPoints);
}

class ControlButton final : public QPushButton {
public:
    explicit ControlButton(bool dot,QWidget* parent):QPushButton(parent),dot_(dot) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAutoDefault(false);
    }
    QString title,detail;
    bool active{};
    int cardWidth(int minimum) const {
        QFont heading=font();heading.setWeight(QFont::DemiBold);heading.setPixelSize(15);
        QFont body=font();body.setPixelSize(13);
        int width=minimum;
        for(const auto& line:title.split('\n'))width=std::max(width,QFontMetrics(heading).horizontalAdvance(line)+28);
        for(const auto& line:detail.split('\n'))width=std::max(width,QFontMetrics(body).horizontalAdvance(line)+28);
        return std::min(width,210);
    }
    int cardHeight(int width) const {
        QFont heading=font();heading.setWeight(QFont::DemiBold);heading.setPixelSize(15);
        QFont body=font();body.setPixelSize(13);
        const int titleHeight=std::min(QFontMetrics(heading).boundingRect(QRect(0,0,width-28,1000),Qt::TextWordWrap,title).height(),QFontMetrics(heading).height()*2);
        const int detailHeight=detail.isEmpty()?0:std::min(QFontMetrics(body).boundingRect(QRect(0,0,width-28,1000),Qt::TextWordWrap,detail).height(),QFontMetrics(body).height()*4)+5;
        return std::max(56,22+titleHeight+detailHeight);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        if(dot_) {
            p.setBrush(QColor(20,20,20,110));p.setPen(QPen(active?accent:QColor("#eeeeee"),2));
            p.drawEllipse(QRectF(rect()).adjusted(7,7,-7,-7));
            if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(accent,1,Qt::DashLine));p.drawEllipse(QRectF(rect()).adjusted(2,2,-2,-2));}
            return;
        }
        const QRectF box=QRectF(rect()).adjusted(1,1,-1,-1);
        p.setBrush(active?accent:surface);
        p.setPen(QPen(hasFocus()||underMouse()?accent:QColor("#292929"),hasFocus()?2:1));
        p.drawRoundedRect(box,3,3);
        const QColor ink=active?QColor("#001e1b"):QColor("#f5f5f5");
        p.setPen(ink);
        QFont heading=font();heading.setWeight(QFont::DemiBold);heading.setPixelSize(15);p.setFont(heading);
        const QFontMetrics headingMetrics(heading);
        const int titleHeight=std::min(headingMetrics.boundingRect(QRect(0,0,width()-28,1000),Qt::TextWordWrap,title).height(),headingMetrics.height()*2);
        p.save();p.setClipRect(QRect(14,12,width()-28,titleHeight));
        p.drawText(QRect(14,12,width()-28,titleHeight),Qt::AlignLeft|Qt::AlignTop|Qt::TextWordWrap,title);p.restore();
        QFont body=font();body.setPixelSize(13);p.setFont(body);
        p.setPen(active?QColor("#003c35"):QColor("#c7c7c7"));
        p.drawText(QRect(14,17+titleHeight,width()-28,height()-29-titleHeight),Qt::AlignLeft|Qt::AlignTop|Qt::TextWordWrap,detail);
    }
private:
    bool dot_;
};
}

DeviceCanvas::DeviceCanvas(QWidget* parent):QWidget(parent),mouse_(assetPixmap(QStringLiteral("side.png"))) {
    setObjectName(QStringLiteral("deviceCanvas"));
    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    rebuild();
}
QSize DeviceCanvas::sizeHint()const{return {740,620};}
QSize DeviceCanvas::minimumSizeHint()const{return {460,420};}
void DeviceCanvas::setMode(DeviceMode mode) {
    if(mode_==mode)return;
    mode_=mode;selected_=-1;labels_.clear();rebuild();
}
void DeviceCanvas::setSelected(int index) {
    selected_=index>=0 && index<cards_.size()?index:-1;
    for(int i=0;i<cards_.size();++i) {
        for(auto* button:{cards_[i],pins_[i]}) {
            static_cast<ControlButton*>(button)->active=i==selected_;
            button->setChecked(i==selected_);button->update();
        }
    }
    update();
}
void DeviceCanvas::setLabels(const QStringList& labels) {
    labels_=labels;
    const auto definitions=points(mode_);
    const QStringList defaults{QStringLiteral("中键"),QStringLiteral("切换滚轮模式"),QStringLiteral("音量升高/降低"),QStringLiteral("前进"),QStringLiteral("后退"),QStringLiteral("手势")};
    for(int i=0;i<cards_.size();++i) {
        auto* card=static_cast<ControlButton*>(cards_[i]);
        const auto caption=QString::fromUtf8(definitions[i].caption);
        card->title=mode_==DeviceMode::Buttons?labels.value(i,defaults.value(i)):caption;
        if(mode_==DeviceMode::Buttons && i==2)card->title.replace(QStringLiteral(" / "),QStringLiteral(" /\n"));
        card->detail=mode_==DeviceMode::Buttons?caption:labels.value(i,QStringLiteral("未读取"));
        const auto description=card->title+QStringLiteral("\n")+card->detail;
        for(auto* button:{cards_[i],pins_[i]}) {
            button->setText(description);button->setToolTip(description);
            button->setAccessibleName(caption);button->setAccessibleDescription(description);
            button->update();
        }
    }
    arrange();
}
void DeviceCanvas::rebuild() {
    qDeleteAll(cards_);qDeleteAll(pins_);cards_.clear();pins_.clear();anchors_.clear();
    const auto definitions=points(mode_);
    for(int i=0;i<static_cast<int>(definitions.size());++i) {
        auto* card=new ControlButton(false,this);auto* pin=new ControlButton(true,this);
        card->setObjectName(QStringLiteral("hotspot_%1").arg(i));
        pin->setObjectName(QStringLiteral("devicePin_%1").arg(i));
        for(auto* button:{card,pin}) {
            button->setCheckable(true);
            connect(button,&QPushButton::clicked,this,[this,i]{activate(i);});button->show();
        }
        cards_.append(card);pins_.append(pin);
        if(i)QWidget::setTabOrder(cards_[i-1],card);
    }
    setLabels(labels_);setSelected(selected_);
}
void DeviceCanvas::activate(int index){setSelected(index);if(selected)selected(index);}
void DeviceCanvas::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);arrange();}
void DeviceCanvas::arrange() {
    const auto definitions=points(mode_);if(cards_.isEmpty())return;
    constexpr int margin=12,gap=8;
    const double ratio=mouse_.isNull()?300.0/481.0:double(mouse_.width())/mouse_.height();
    // Match the original native scene: image-centred labels sit beside their
    // physical controls, including the Back label underneath its pin. Wider
    // windows add breathing room instead of stretching leaders across the page.
    const double imageWidth=std::max(1.0,std::min({double(width()-220),482.0*ratio,double(height()-84)*ratio}));
    const double imageHeight=imageWidth/ratio;
    image_=QRectF((width()-imageWidth)/2,(height()-imageHeight)/2,imageWidth,imageHeight);
    anchors_.clear();
    QVector<QRect> positions;
    for(int i=0;i<cards_.size();++i) {
        anchors_.append(QPointF(image_.left()+definitions[i].x*image_.width(),image_.top()+definitions[i].y*image_.height()));
        pins_[i]->setGeometry(qRound(anchors_[i].x())-14,qRound(anchors_[i].y())-14,28,28);
        auto* card=static_cast<ControlButton*>(cards_[i]);
        const int w=card->cardWidth(definitions[i].width),h=card->cardHeight(w);
        const auto pin=anchors_[i];
        const int x=qRound(definitions[i].align<0?pin.x()-w-24:definitions[i].align==0?pin.x()-w/2+4:pin.x()+28);
        const int y=qRound(definitions[i].align==0?pin.y()+29:pin.y()-h/2);
        positions.append(QRect(std::clamp(x,margin,std::max(margin,width()-margin-w)),std::clamp(y,margin,std::max(margin,height()-margin-h)),w,h));
    }
    QVector<int> order;
    for(int i=0;i<cards_.size();++i)order.append(i);
    std::ranges::stable_sort(order,[&](int a,int b){return positions[a].top()<positions[b].top();});
    for(int n=0;n<order.size();++n) {
        auto& box=positions[order[n]];
        for(int previous=0;previous<n;++previous) {
            const auto& above=positions[order[previous]];
            if(box.adjusted(-gap,-gap,gap,gap).intersects(above))box.moveTop(above.bottom()+gap+1);
        }
    }
    for(int i=0;i<cards_.size();++i)cards_[i]->setGeometry(positions[i]);
    update();
}
void DeviceCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);p.fillRect(rect(),background);p.setRenderHint(QPainter::Antialiasing);p.setRenderHint(QPainter::SmoothPixmapTransform);
    if(!mouse_.isNull())p.drawPixmap(image_,mouse_,QRectF(mouse_.rect()));
    for(int i=0;i<cards_.size();++i) {
        const auto box=cards_[i]->geometry();
        const QPointF edge(std::clamp(anchors_[i].x(),double(box.left()),double(box.right())),std::clamp(anchors_[i].y(),double(box.top()),double(box.bottom())));
        p.setPen(QPen(i==selected_?accent:QColor("#494949"),1));p.drawLine(edge,anchors_[i]);
    }
}
}
