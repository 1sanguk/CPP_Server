#include <log_list.h>

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QScrollBar>

namespace{
const QColor kHighlight(255, 214, 64);
const QColor kDefaultText(242, 242, 242);
constexpr int kColorRole = Qt::UserRole + 1;
}

LogList::LogList(int max_lines, QWidget* parent) : QListWidget(parent), max_lines(max_lines){
    setObjectName("logList");
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformItemSizes(true);
    setWordWrap(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setToolTip("줄을 선택하고 Ctrl+C로 복사할 수 있습니다.");
}

void LogList::Add_Line(const QString& text, const QColor& color, bool bold){
    // 사용자가 위로 올려 보는 중이면 끌어내리지 않고, 맨 아래를 보고 있을 때만 따라 내려간다.
    QScrollBar* bar = verticalScrollBar();
    const bool follow = bar->value() >= bar->maximum() - 2;

    auto* item = new QListWidgetItem(text);
    // 검색 강조(노란 바탕)를 풀 때 원래 글자색으로 되돌리려고 따로 기억해 둔다.
    item->setData(kColorRole, color.isValid() ? color : QColor(kDefaultText));
    item->setForeground(item->data(kColorRole).value<QColor>());
    if(bold){
        QFont font = item->font();
        font.setBold(true);
        item->setFont(font);
    }
    addItem(item);
    if(Apply_To(item)) ++match_count;

    while(count() > max_lines){
        QListWidgetItem* oldest = takeItem(0);
        if(!query.isEmpty() && Matches(oldest)) --match_count;
        delete oldest;
    }
    if(follow) scrollToBottom();
}

int LogList::Apply_Search(const QString& new_query, bool new_only_matches){
    query = new_query;
    only_matches = new_only_matches;
    match_count = 0;
    for(int i = 0; i < count(); ++i){
        if(Apply_To(item(i))) ++match_count;
    }
    if(!query.isEmpty() && !only_matches){
        // 첫 번째로 찾은 줄이 보이게 한다.
        for(int i = 0; i < count(); ++i){
            if(Matches(item(i))){
                scrollToItem(item(i));
                break;
            }
        }
    }else{
        scrollToBottom();
    }
    return match_count;
}

bool LogList::Matches(const QListWidgetItem* item) const{
    return !query.isEmpty() && item->text().contains(query, Qt::CaseInsensitive);
}

bool LogList::Apply_To(QListWidgetItem* item) const{
    const bool match = Matches(item);
    // 어두운 바탕이라 일치한 줄은 노란 바탕 + 검은 글씨로, 아닌 줄은 원래 색으로 그린다.
    item->setBackground(match ? QBrush(kHighlight) : QBrush());
    item->setForeground(match ? QColor(Qt::black) : item->data(kColorRole).value<QColor>());
    item->setHidden(only_matches && !query.isEmpty() && !match);
    return match;
}

void LogList::keyPressEvent(QKeyEvent* event){
    if(event->matches(QKeySequence::Copy)){
        QStringList lines;
        for(int i = 0; i < count(); ++i){
            if(item(i)->isSelected() && !item(i)->isHidden()) lines.push_back(item(i)->text());
        }
        if(!lines.isEmpty()) QApplication::clipboard()->setText(lines.join('\n'));
        return;
    }
    QListWidget::keyPressEvent(event);
}
