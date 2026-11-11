#include "device_canvas.hpp"

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
struct Point { double x, y; bool left; const char* caption; };
constexpr std::array buttonPoints{
    Point{.71,.15,false,"滚轮按钮"}, Point{.81,.34,false,"顶部按钮"},
    Point{.55,.51,false,"拇指滚轮"}, Point{.35,.43,true,"前进按钮"},
    Point{.45,.60,true,"后退按钮"}, Point{.08,.58,true,"拇指按钮"}};
constexpr std::array pointerPoints{
    Point{.73,.16,false,"滚轮"}, Point{.83,.54,false,"指针速度"},
    Point{.55,.51,true,"拇指滚轮"}};
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
    int cardHeight(int width) const {
        QFont heading=font();heading.setBold(true);heading.setPixelSize(16);
        QFont body=font();body.setPixelSize(13);
        const int titleHeight=std::min(QFontMetrics(heading).boundingRect(QRect(0,0,width-28,1000),Qt::TextWordWrap,title).height(),QFontMetrics(heading).height()*2);
        const int detailHeight=detail.isEmpty()?0:std::min(QFontMetrics(body).boundingRect(QRect(0,0,width-28,1000),Qt::TextWordWrap,detail).height(),QFontMetrics(body).height()*4)+5;
        return std::max(64,24+titleHeight+detailHeight);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        if(dot_) {
            p.setBrush(background);p.setPen(QPen(active?accent:QColor("#eeeeee"),2));
            p.drawEllipse(QRectF(rect()).adjusted(6,6,-6,-6));
            if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(accent,1,Qt::DashLine));p.drawEllipse(QRectF(rect()).adjusted(2,2,-2,-2));}
            return;
        }
        const QRectF box=QRectF(rect()).adjusted(1,1,-1,-1);
        p.setBrush(active?accent:surface);
        p.setPen(QPen(hasFocus()||underMouse()?accent:QColor("#303030"),hasFocus()?2:1));
        p.drawRoundedRect(box,5,5);
        const QColor ink=active?QColor("#001e1b"):QColor("#f5f5f5");
        p.setPen(ink);
        QFont heading=font();heading.setBold(true);heading.setPixelSize(16);p.setFont(heading);
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

DeviceCanvas::DeviceCanvas(QWidget* parent):QWidget(parent),mouse_(QStringLiteral(":/assets/side.png")) {
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
    constexpr int margin=16,gap=14;
    const int cardWidth=std::clamp((width()-180)/2,128,184);
    const double ratio=mouse_.isNull()?300.0/481.0:double(mouse_.width())/mouse_.height();
    const double imageWidth=std::max(1.0,std::min({300.0,double(width()-2*cardWidth-2*margin-2*gap),double(height()-60)*ratio}));
    const double imageHeight=imageWidth/ratio;
    image_=QRectF((width()-imageWidth)/2,(height()-imageHeight)/2,imageWidth,imageHeight);
    anchors_.clear();
    for(int i=0;i<cards_.size();++i) {
        anchors_.append(QPointF(image_.left()+definitions[i].x*image_.width(),image_.top()+definitions[i].y*image_.height()));
        pins_[i]->setGeometry(qRound(anchors_[i].x())-14,qRound(anchors_[i].y())-14,28,28);
    }
    for(bool left:{true,false}) {
        QVector<int> indices;
        for(int i=0;i<cards_.size();++i)if(definitions[i].left==left)indices.append(i);
        std::ranges::sort(indices,[&](int a,int b){return anchors_[a].y()<anchors_[b].y();});
        int bottom=margin-gap;
        for(const int i:indices) {
            const int h=static_cast<ControlButton*>(cards_[i])->cardHeight(cardWidth);
            const int y=std::max(bottom+gap,qRound(anchors_[i].y())-h/2);
            cards_[i]->setGeometry(left?margin:width()-margin-cardWidth,y,cardWidth,h);bottom=y+h;
        }
        int top=height()-margin+gap;
        for(auto it=indices.crbegin();it!=indices.crend();++it) {
            auto* card=cards_[*it];const int y=std::min(card->y(),top-gap-card->height());
            card->move(card->x(),y);top=y;
        }
    }
    update();
}
void DeviceCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);p.fillRect(rect(),background);p.setRenderHint(QPainter::Antialiasing);p.setRenderHint(QPainter::SmoothPixmapTransform);
    if(!mouse_.isNull())p.drawPixmap(image_,mouse_,QRectF(mouse_.rect()));
    const auto definitions=points(mode_);
    for(int i=0;i<cards_.size();++i) {
        const auto box=cards_[i]->geometry();
        const QPointF edge(definitions[i].left?box.right():box.left(),std::clamp(anchors_[i].y(),double(box.top()+12),double(box.bottom()-12)));
        p.setPen(QPen(i==selected_?accent:QColor("#626262"),1,Qt::DashLine));p.drawLine(edge,anchors_[i]);
    }
}
}
