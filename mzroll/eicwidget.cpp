#include "eicwidget.h"

EicWidget::EicWidget(QWidget *p) {

	parent = p; 

	//default values
	_slice = mzSlice(0,0.01,0,0.01);
	_zoomFactor = 0.5;
	_minX = _minY = 0;
	_maxX = _maxY = 0;

	setScene(new QGraphicsScene(this));

	_barplot = NULL;
	_boxplot = NULL;
	_isotopeplot=NULL;
	_focusLine = NULL;
	_selectionLine = NULL;
	_statusText=NULL;

	selectedGroup = NULL;

	autoZoom(true);
	showPeaks(true);
	showSpline(false);
    showBaseLine(true);
	showTicLine(false);
	showNotes(true);
	showIsotopePlot(true);
	showBarPlot(true);
	showBoxPlot(false);

	//scene()->setItemIndexMethod(QGraphicsScene::NoIndex);
	scene()->setItemIndexMethod(QGraphicsScene::BspTreeIndex);
	setDragMode(QGraphicsView::RubberBandDrag);
	setCacheMode(CacheBackground);
	setMinimumSize(QSize(1,1));
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	_showMergedEIC=false;
	_frozen=false;
	_freezeTime=0;
	_timerId=0;
	connect(scene(), SIGNAL(selectionChanged()), SLOT(selectionChangedAction()));

}

EicWidget::~EicWidget() {
	cleanup();
	scene()->clear();
}

void EicWidget::mousePressEvent(QMouseEvent *event) {
	//qDebug << "EicWidget::mousePressEvent(QMouseEvent *event)";
	//setFocus();

	_lastClickPos = event->pos();
	QGraphicsView::mousePressEvent(event);

    if (event->button() == Qt::LeftButton) {
		_mouseStartPos = event->pos();
	} 
}

void EicWidget::mouseReleaseEvent(QMouseEvent *event) {
 //qDebug <<" EicWidget::mouseReleaseEvent(QMouseEvent *event)";
    QGraphicsView::mouseReleaseEvent(event);


    //int selectedItemCount = scene()->selectedItems().size();
    mzSlice bounds = visibleEICBounds();

    _mouseEndPos	= event->pos();
    float rtmin = invX( std::min(_mouseStartPos.x(), _mouseEndPos.x()) );
    float rtmax = invX( std::max(_mouseStartPos.x(), _mouseEndPos.x()) );
    int deltaX =  _mouseEndPos.x() - _mouseStartPos.x();
    _mouseStartPos = _mouseEndPos; //

    //user is holding shift while releasing the mouse.. integrate area
    if (_areaIntegration ||  (event->button() == Qt::LeftButton && event->modifiers() == Qt::ShiftModifier) ) {
        toggleAreaIntegration(false);
        //minimum size for region to integrate is 0.01 seconds
        if(rtmax-rtmin> 0.01) integrateRegion(rtmin, rtmax);

    } else if (_spectraAveraging || ( event->button() == Qt::LeftButton && event->modifiers() == Qt::ControlModifier) ) {
         toggleSpectraAveraging(false);
         getMainWindow()->getSpectraWidget()->constructAverageScan(rtmin,rtmax);

    } else if (abs(deltaX) > 10 && event->button() == Qt::LeftButton) {    //user released button and no items are selected
        if ( deltaX > 0  ) {  //zoom in
            _slice.rtmin = rtmin;
            _slice.rtmax = rtmax;
            if ( selectedGroup ) {
                if (selectedGroup->meanRt > rtmin && selectedGroup->meanRt < rtmax ) {
                    float d = std::max(abs(selectedGroup->meanRt - rtmin), abs(selectedGroup->meanRt - rtmax));
                    _slice.rtmin = selectedGroup->meanRt - d;
                    _slice.rtmax = selectedGroup->meanRt + d;
                }
            }
        } else if ( deltaX < 0 ) {	 //zoomout
            //zoom(_zoomFactor * 1.2 );
            _slice.rtmin *= 0.8;
            _slice.rtmax *= 1.22;
            if ( _slice.rtmin < bounds.rtmin) _slice.rtmin=bounds.rtmin;
            if ( _slice.rtmax > bounds.rtmax) _slice.rtmax=bounds.rtmax;
        }
        replot(selectedGroup);
    }
}

void EicWidget::integrateRegion(float rtmin, float rtmax) {
 //qDebug <<" EicWidget::integrateRegion(float rtmin, float rtmax)";
	//qDebug << "Integrating area from " << rtmin << " to " << rtmax;
	this->_integratedGroup.clear();
	this->_integratedGroup.compound = _slice.compound;
	this->_integratedGroup.srmId = _slice.srmId;

	for(int i=0; i < eics.size(); i++ ) {
		EIC* eic = eics[i];
		Peak peak(eic,0);

		for( int j=0; j < eic->size(); j++) {
			if(eic->rt[j] >= rtmin && eic->rt[j] <= rtmax) {
				if(peak.minpos==0) { peak.minpos=j;  peak.rtmin=eic->rt[j]; }
				if(peak.maxpos< j) { peak.maxpos=j;  peak.rtmax=eic->rt[j]; }
				peak.peakArea += eic->intensity[j];
				peak.rtmin=rtmin;
				peak.rtmax=rtmax;
				peak.mzmin=this->_slice.mzmin;
				peak.mzmax=this->_slice.mzmax;

				if(eic->intensity[j]> peak.peakIntensity) {
					peak.peakIntensity=eic->intensity[j];
					peak.pos=j;
					peak.rt=eic->rt[j];
					peak.peakMz = eic->mz[j];
				}
			}
		}
		if (peak.pos > 0) {
			//qDebug << "details" << peak.pos << " " << peak.minpos << " " << peak.maxpos;
			eic->getPeakDetails(peak);
			_integratedGroup.addPeak(peak);
            qDebug() << eic->sampleName.c_str() << " " << peak.peakArea << " " << peak.peakAreaCorrected;
			this->showPeakArea(&peak);
		}
	}


	_integratedGroup.groupStatistics();
	setSelectedGroup(&_integratedGroup);
	scene()->update();
	this->copyToClipboard();


        getMainWindow()->bookmarkPeakGroup();
}

void EicWidget::mouseDoubleClickEvent(QMouseEvent* event){
 //qDebug <<" EicWidget::mouseDoubleClickEvent(QMouseEvent* event)";
	QGraphicsView::mouseDoubleClickEvent(event);

	QPointF pos = event->pos();
	float rt = invX(pos.x());

	vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
	Scan* selScan = NULL; float minDiff = FLT_MAX;
	for(int i=0; i < samples.size(); i++ ) {
		for(int j=0; j < samples[i]->scans.size(); j++ ) {
				//if ( samples[i]->scans[j]->mslevel != 1) continue;
				float diff = abs(samples[i]->scans[j]->rt - rt);
				if (  diff < minDiff ) { 
					minDiff = diff;
					selScan = samples[i]->scans[j];
				}
		}
	}

	if (selScan != NULL) { 
        setFocusLine(selScan->rt);
		getMainWindow()->spectraWidget->setScan(selScan);
	}
}

void EicWidget::selectionChangedAction() {
 //qDebug <<" EicWidget::selectionChangedAction()";
	QList<QGraphicsItem*>items = scene()->selectedItems();
	if (items.size()) { 
		if (QGraphicsPixmapItem *note = qgraphicsitem_cast<QGraphicsPixmapItem *>(items[0])) {
			QVariant v = note->data(0);
			int noteid = v.value<int>();
			getMainWindow()->notesDockWidget->showNote(noteid);
		}
	}
}

void EicWidget::setFocusLine(float rt) { 
 //qDebug <<" EicWidget::setFocusLine(float rt)";
	_focusLineRt = rt;
	if (_focusLine == NULL ) _focusLine = new QGraphicsLineItem(0,scene());
	if (_focusLine->scene() != scene() ) scene()->addItem(_focusLine);

	QPen pen(Qt::red, 2, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
	_focusLine->setPen(pen);
	_focusLine->setLine(toX(rt), 0, toX(rt), height() );
}


void EicWidget::drawSelectionLine(float rtmin, float rtmax) { 
 //qDebug <<" EicWidget::drawSelectionLine(float rtmin, float rtmax)";
	if (_selectionLine == NULL ) _selectionLine = new QGraphicsLineItem(0,scene());
	if (_selectionLine->scene() != scene() ) scene()->addItem(_selectionLine);

	QPen pen(Qt::red, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	_selectionLine->setPen(pen);
    _selectionLine->setZValue(1000);
	_selectionLine->setLine(toX(rtmin), height()-20, toX(rtmax), height()-20 );
    _selectionLine->update();
}



void EicWidget::setScan(Scan* scan) {
 //qDebug <<" EicWidget::setScan(Scan* scan)";
	if ( scan == NULL ) return;
	getMainWindow()->spectraWidget->setScan(scan,_slice.rtmin-5, _slice.rtmax+5);
	 


}

void EicWidget::mouseMoveEvent(QMouseEvent* event){
// //qDebug <<" EicWidget::mouseMoveEvent(QMouseEvent* event)";
    QGraphicsView::mouseMoveEvent(event);
    //QPoint posi = event->globalPos();
    //QToolTip::showText(posi, "(" + QString::number(rt,'f',4) + " " + QString::number(intensity,'f',2) + ")" , this);
    if (_mouseStartPos !=_mouseEndPos ) {

        if ( event->modifiers() == Qt::ShiftModifier ) {
    	    QPointF pos = event->pos();
    	    float rt = invX(pos.x());
            float rtA = invX(_mouseStartPos.x());
            drawSelectionLine(min(rt,rtA), max(rt,rtA));
        }

    }
}

void EicWidget::cleanup() {
 //qDebug <<" EicWidget::cleanup()";
	//remove groups
	delete_all(eics);
	eics.clear();
	peakgroups.clear();
	if (_showTicLine == false && tics.size() > 0 ) { delete_all(tics); tics.clear(); }
	clearPlot();
}

void EicWidget::addPeakGroup(PeakGroup& group) { 
 //qDebug <<" EicWidget::addPeakGroup(PeakGroup& group)";
	peakgroups.push_back(group);
}

void EicWidget::computeEICs() {
 	qDebug() << " EicWidget::computeEICs()";
	vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
    	if (samples.size() == 0) return;

    	QSettings *settings 		= getMainWindow()->getSettings();
        float eic_smoothingWindow = settings->value("eic_smoothingWindow").toDouble();
        int   eic_smoothingAlgorithm = settings->value("eic_smoothingAlgorithm").toInt();
        float amuQ1 = settings->value("amuQ1").toDouble();
        float amuQ3 = settings->value("amuQ3").toDouble();
        int baseline_smoothing = settings->value("baseline_smoothing").toInt();
        int baseline_quantile =  settings->value("baseline_quantile").toInt();


        //qDebug << "eic_smoothingAlgorithm=" << eic_smoothingAlgorithm;

	mzSlice slice = _slice;
	mzSlice bounds  = visibleSamplesBounds();
	slice.rtmin=bounds.rtmin;
	slice.rtmax=bounds.rtmax;

	//get eics
        eics = PeakDetector::pullEICs(&slice,
                                              samples,
                                              EicLoader::PeakDetection,
                                              eic_smoothingWindow,
                                              eic_smoothingAlgorithm,
                                              amuQ1,
                                              amuQ3,
                                              baseline_smoothing,
                                              baseline_quantile);

	//find peaks
	//for(int i=0; i < eics.size(); i++ )  eics[i]->getPeakPositions(eic_smoothingWindow);
	//for(int i=0; i < eics.size(); i++ ) mzUtils::printF(eics[i]->intensity);
	////qDebug << tr("computeEICs() Done. ElepsTime=%1 msec").arg(timer.elapsed());

	//group peaks
	groupPeaks();

	////qDebug << tr("computeEICs() Done. ElepsTime=%1 msec").arg(timer.elapsed());
	//associate compound names with peak groups
	if (_slice.compound)  for(int i=0; i < peakgroups.size(); i++ ) peakgroups[i].compound = _slice.compound; 
	if (!_slice.srmId.empty()) for(int i=0; i < peakgroups.size(); i++ ) peakgroups[i].srmId = _slice.srmId;
}

mzSlice EicWidget::visibleEICBounds() {
    mzSlice bounds(0,0,0,0);

    for(int i=0; i < eics.size(); i++ ) {
        EIC* eic = eics[i];
        if( i == 0 || eic->rtmin < bounds.rtmin ) bounds.rtmin = eic->rtmin;
        if( i == 0 || eic->rtmax > bounds.rtmax ) bounds.rtmax = eic->rtmax;
        if( i == 0 || eic->mzmin < bounds.mzmin ) bounds.mzmin = eic->mzmin;
        if( i == 0 || eic->mzmax > bounds.mzmax ) bounds.mzmax = eic->mzmax;
        if( i == 0 || eic->maxIntensity > bounds.ionCount) bounds.ionCount = eic->maxIntensity;
    }
        return bounds;
    //} Feng note: move this bracket to above "return bounds" fixes a maximum retention time bug.
}


mzSlice EicWidget::visibleSamplesBounds() {
 //qDebug <<"EicWidget::visibleSamplesBounds() ";
    mzSlice bounds(0,0,0,0);
    vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
    for(int i=0; i < samples.size(); i++ ) {
        mzSample* sample = samples[i];
        if( i == 0 || sample->minRt < bounds.rtmin ) bounds.rtmin = sample->minRt;
        if( i == 0 || sample->maxRt > bounds.rtmax ) bounds.rtmax = sample->maxRt;
        if( i == 0 || sample->minMz < bounds.mzmin ) bounds.mzmin = sample->minMz;
        if( i == 0 || sample->maxMz > bounds.mzmax ) bounds.mzmax = sample->maxMz;
        if( i == 0 || sample->maxIntensity > bounds.ionCount) bounds.ionCount = sample->maxIntensity;
    }
    return bounds;
}

void EicWidget::findPlotBounds() {
 //qDebug <<" EicWidget::findPlotBounds()";

    //mzSlice bounds = visibleEICBounds();

    //get bounds
    if ( eics.size() == 0 ) return;
    if ( _slice.rtmin == 0 && _slice.rtmax == 0 ) return;
    //qDebug() << "EicWidget::findPlotBounds()";

    _minX = _slice.rtmin;
    _maxX = _slice.rtmax;

    _minY = 0;
    _maxY = 0;       //intensity
   
    /*/
    //full proof version    
    for(int i=0; i < eics.size(); i++ ) {
        EIC* eic = eics[i];
        for (int j=0; j < eic->size(); j++ ){
            if ( eic->rt[j] < _slice.rtmin) continue;
            if ( eic->rt[j] > _slice.rtmax) continue;
            if ( eic->intensity[j] > _maxY ) _maxY=eic->intensity[j];
        }
    }
	*/

    //approximate version
    for(int i=0; i < peakgroups.size(); i++ ) {
	    if ( mzUtils::checkOverlap(peakgroups[i].minRt, peakgroups[i].maxRt, _slice.rtmin, _slice.rtmax) > 0) {
		    if( peakgroups[i].maxIntensity > _maxY ) {
			    _maxY = peakgroups[i].maxIntensity;
		    }
	    }
    }

    //if(_minY <= 0) _minY = 0;
    _maxY = (_maxY * 1.3) + 1;
    if (_minX > _maxX) swap(_minX,_maxX);
    //qDebug() << "EicWidget::findPlotBounds()" << _slice.rtmin << " " << _slice.rtmax << " " << _minY << " " << _maxY;
}

float EicWidget::toX(float x) {
    if(_minX ==_maxX || x < _minX || x > _maxX) return 0;
    return( (x-_minX)/(_maxX-_minX)*scene()->width());
}

float EicWidget::toY(float y) {
    if(_minY == _maxY || y < _minY || y > _maxY) return 0;
    return( scene()->height()- ((y-_minY)/(_maxY-_minY) * scene()->height()));
}

float EicWidget::invX(float x) {
    if (_minX == _maxX) return 0;
     return  (x/scene()->width()) * (_maxX-_minX) + _minX;
}

float EicWidget::invY(float y) {
     if(_minY == _maxY) return 0;
     return  -1*((y-scene()->height())/scene()->height() * (_maxY-_minY) + _minY);

}

void EicWidget::replotForced() {
 //qDebug <<" EicWidget::replotForced()";
    if (isVisible() ) {
        recompute();
	replot();

	}
}

void EicWidget::replot() {
 //qDebug <<" EicWidget::replot()";
    if ( isVisible() ) { replot(getSelectedGroup()); }
}

void EicWidget::addEICLines(bool showSpline) {	
 //qDebug <<" EicWidget::addEICLines(bool showSpline)";

    //sort eics by peak height of selected group
    vector<Peak> peaks;
    if (getSelectedGroup()) { 
        PeakGroup* group=getSelectedGroup(); 
        peaks=group->getPeaks(); 
        sort(peaks.begin(), peaks.end(), Peak::compIntensity); 
    } else {
        std::sort(eics.begin(), eics.end(), EIC::compMaxIntensity);
    }


    //display eics
    for( unsigned int i=0; i< eics.size(); i++ ) {
        EIC* eic = eics[i];
        if (eic->size()==0) continue;
        if (eic->sample != NULL && eic->sample->isSelected == false) continue;
        if (eic->maxIntensity <= 0) continue;
        EicLine* line = new EicLine(0,scene());



        //sample stacking..
        int zValue=0;
        for(int j=0; j < peaks.size(); j++ ) {
            if (peaks[j].getSample() == eic->getSample()) { zValue=j; break; }
        }

        //ignore EICs that do not fall within current time range
        for (int j=0; j < eic->size(); j++ ){
            if ( eic->rt[j] < _slice.rtmin) continue;
            if ( eic->rt[j] > _slice.rtmax) continue;
            if ( showSpline ) {
                line->addPoint(QPointF( toX(eic->rt[j]), toY(eic->spline[j])));
            } else {
                line->addPoint(QPointF( toX(eic->rt[j]), toY(eic->intensity[j])));
            }
        }
        QColor pcolor = QColor::fromRgbF( eic->color[0], eic->color[1], eic->color[2], 0.5 ).darker();
        QPen pen(pcolor, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        QColor bcolor = QColor::fromRgbF( eic->color[0], eic->color[1], eic->color[2], 0.5 );
        QBrush brush(bcolor);

        //brush.setStyle(Qt::NoBrush);
        brush.setStyle(Qt::SolidPattern)		;

        line->setZValue(zValue);
        line->setFillPath(true);
        line->setEIC(eic);
        line->setBrush(brush);
        line->setPen(pen);
        line->setColor(pcolor);


        //line->fixEnds();
    }
}

void EicWidget::addTicLine() {
 //qDebug <<" EicWidget::addTicLine()";

	vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
	if ( tics.size() == 0  || tics.size() != samples.size() ) {
		delete_all(tics);
		for(int i=0; i < samples.size(); i++ )  {
            int mslevel=1;
            //attempt at automatically detecting correct scan type for construstion of TIC

            if  (samples[i]->scans.size() > 0) mslevel=samples[i]->scans[0]->mslevel;
            EIC* tic = samples[i]->getTIC(0,0,mslevel);
			if (tic != NULL) tics.push_back(tic);
		}
	}

	float tmpMaxY = _maxY;
	float tmpMinY = _minY;

	for( unsigned int i=0; i< tics.size(); i++ ) {
		EIC* tic = tics[i];
		if (tic->size()==0) continue;
        if (tic->sample != NULL && tic->sample->isSelected == false) continue;
                EicLine* line = new EicLine(0,scene());
		line->setEIC(tic);

		_maxY = tic->maxIntensity;
		_minY = 0;

		for (int j=0; j < tic->size(); j++ ){
			if ( tic->rt[j] < _slice.rtmin) continue;
			if ( tic->rt[j] > _slice.rtmax) continue;
			line->addPoint(QPointF( toX(tic->rt[j]), toY(tic->intensity[j])));
		}

		mzSample* s= tic->sample;
                QColor color = QColor::fromRgbF( s->color[0], s->color[1], s->color[2], s->color[3] );
                line->setColor(color);

		QBrush brush(color,Qt::Dense5Pattern);
		line->setBrush(brush);
		line->setFillPath(true);
		line->setZValue(-i);	//tic should not be in foreground

		QPen pen = Qt::NoPen;
		line->setPen(pen);

               //line->fixEnds();
            }

	//restore min and max Y
	_maxY = tmpMaxY; _minY = tmpMinY;
}

void EicWidget::addMergedEIC() {
 //qDebug <<" EicWidget::addMergedEIC()";

    QSettings* settings  = this->getMainWindow()->getSettings();
    int eic_smoothingWindow = settings->value("eic_smoothingWindow").toInt();
    int eic_smoothingAlgorithm = settings->value("eic_smoothingAlgorithm").toInt();
    int baseline_smoothing = settings->value("baseline_smoothing").toInt();
    int baseline_quantile =  settings->value("baseline_quantile").toInt();


    EicLine* line = new EicLine(0,scene());

    EIC* eic = EIC::eicMerge(eics);
    eic->setSmootherType((EIC::SmootherType) eic_smoothingAlgorithm);
    eic->setBaselineSmoothingWindow(baseline_smoothing);
    eic->setBaselineDropTopX(baseline_quantile);
    eic->getPeakPositions(eic_smoothingWindow);

    for (int j=0; j < eic->size(); j++ ){
        if ( eic->rt[j] < _slice.rtmin) continue;
        if ( eic->rt[j] > _slice.rtmax ) continue;
        line->addPoint(QPointF( toX(eic->rt[j]), toY(eic->spline[j])));
    }

    QColor color = QColor::fromRgbF(0.1,0.1,0.1,1 );
    QPen pen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QBrush brush(color);

    line->setEIC(eic);
    line->setBrush(brush);
    line->setPen(pen);
    line->setColor(color);
}

void EicWidget::addBaseLine() {
    qDebug() << " EicWidget::addBaseLine()";
    QSettings* settings  = this->getMainWindow()->getSettings();
    int baseline_smoothing = settings->value("baseline_smoothing").toInt();
    int baseline_quantile =  settings->value("baseline_quantile").toInt();

    for( unsigned int i=0; i< eics.size(); i++ ) {
        EIC* eic = eics[i];
        eic->computeBaseLine(baseline_smoothing,baseline_quantile);
        if (eic->size()==0) continue;
        EicLine* line = new EicLine(0,scene());
        line->setEIC(eic);

        float baselineSum=0;
        for (int j=0; j < eic->size(); j++ ){
            if ( eic->rt[j] < _slice.rtmin) continue;
            if ( eic->rt[j] > _slice.rtmax ) continue;
            baselineSum += eic->baseline[j];
            line->addPoint(QPointF( toX(eic->rt[j]), toY(eic->baseline[j])));
        }

        if ( baselineSum == 0 ) continue;

        QColor color = Qt::black; //QColor::fromRgbF( eic->color[0], eic->color[1], eic->color[2], 0.9 );
        line->setColor(color);

        //QPen pen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        QBrush brush(Qt::black);
        brush.setStyle(Qt::Dense4Pattern);
        line->setBrush(brush);
        line->setFillPath(true);

        QPen pen = Qt::NoPen;
        line->setPen(pen);
        //line->fixEnds();
    }
}

void EicWidget::showPeakArea(Peak* peak) {
 //qDebug <<" EicWidget::showPeakArea(Peak* peak)";

    if (peak == NULL ) return;
    if( peak->hasEIC()==false) return;

    float rtWidth = peak->rtmax-peak->rtmin;

    //make sure that this is not a dead pointer to lost eic
    bool matched=false;
    EIC* eic= peak->getEIC();
    for(int i=0; i < eics.size(); i++) if (eics[i]== eic) {matched=true; break; }
    if(!matched) return;

    //get points around the peak.
    vector<mzPoint>observed = eic->getIntensityVector(*peak);
    if(observed.size() == 0 ) return;

    EicLine* line = new EicLine(0,scene());
    line->setClosePath(true);
    for(int i=0; i < observed.size(); i++ ) {
        line->addPoint(QPointF( toX(observed[i].x), toY(observed[i].y)));
    }
    QColor color = Qt::black;
    line->setColor(color);
    QBrush brush(color,Qt::CrossPattern);
    line->setBrush(brush);

    //QPen pen(Qt::black,Qt::SolidLine);
    //pen.setWidth(3);
    //line->setPen(pen);
}

void EicWidget::setupColors() { 
 //qDebug <<" EicWidget::setupColors()";
	for( unsigned int i=0; i< eics.size(); i++ ) {
		EIC* eic = eics[i];
		if (eic == NULL ) continue;
        if (eic->sample != NULL)  {
            eic->color[0] = eic->sample->color[0];
            eic->color[1] = eic->sample->color[1];
            eic->color[2] = eic->sample->color[2];
            eic->color[3] = eic->sample->color[3];
        } else {
            eic->color[0] = 0.6;
            eic->color[1] = 0.1;
            eic->color[2] = 0.1;
            eic->color[3] = 0;
        }
	}
}

void EicWidget::clearPlot() { 
 //qDebug <<" EicWidget::clearPlot()";
	if(_isotopeplot && _isotopeplot->scene()) { _isotopeplot->clear(); scene()->removeItem(_isotopeplot); }
	if(_barplot && _barplot->scene()) { _barplot->clear(); scene()->removeItem(_barplot);  }
	if(_boxplot && _boxplot->scene()) { _boxplot->clear(); scene()->removeItem(_boxplot);  }
	if(_focusLine && _focusLine->scene()) {scene()->removeItem(_focusLine); }
	if(_selectionLine && _selectionLine->scene()) {scene()->removeItem(_selectionLine); }
	if(_statusText && _statusText->scene()) { scene()->removeItem(_statusText); }
	scene()->clear();
	scene()->setSceneRect(10,10,this->width()-10, this->height()-10);	
}

void EicWidget::replot(PeakGroup* group ) {
 //qDebug <<" EicWidget::replot(PeakGroup* group )";

    if (eics.size() == 0 ) return;
    if ( _slice.rtmin <= 0 && _slice.rtmax <= 0 ) return;

    findPlotBounds(); //plot xmin and xmax etc..
    setupColors();	  //associate color with each EIC

    //qDebug << "EicWidget::replot() " << " group=" << group << " mz: " << _slice.mzmin << "-" << _slice.mzmax << " rt: " << _slice.rtmin << "-" << _slice.rtmax;
    clearPlot();

    //score peak quality
    Classifier* clsf = getMainWindow()->getClassifier();
    if (clsf != NULL) {
        for(int i=0; i<peakgroups.size(); i++) {
            clsf->classify(&peakgroups[i]);
            peakgroups[i].updateQuality();
        }
    }


    setSelectedGroup(group);
    setTitle();
    addEICLines(_showSpline);
    showAllPeaks();

    if (group) {
        if(group->compound != NULL && group->compound->expectedRt>0)
            _focusLineRt=group->compound->expectedRt;
    }

    if(_showBaseline)  addBaseLine();
    if(_showTicLine)   addTicLine();
    if(_showMergedEIC) addMergedEIC();
    if(_focusLineRt >0) setFocusLine(_focusLineRt);
    if(_showNotes)	 getNotes(_slice.mzmin,_slice.mzmax);	//get notes that fall withing this mzrange
    addAxes();


   //setStatusText("Unknown Expected Retention Time!");

    getMainWindow()->addToHistory(_slice);
    scene()->update();

    //qDebug << "\t Number of eics " << eics.size();
    //qDebug << "\t Number of peakgroups " << peakgroups.size();
    //qDebug << "\t Number of graphic objects " << scene()->items().size();
    //qDebug << "\t BSP Depth " << scene()->bspTreeDepth();
    //qDebug << "\t EicWidget::replot() done ";
}	

void EicWidget::setTitle() {
 //qDebug <<" EicWidget::setTitle()";
	QFont font("Helvetica");
        int pxSize = scene()->height()*0.03;
        if ( pxSize < 14 ) pxSize = 14;
        if ( pxSize > 20 ) pxSize = 20;
	font.setPixelSize(pxSize);

        QString tagString;

        if (selectedGroup != NULL ) {
            tagString = QString(selectedGroup->getName().c_str());
        } else if (_slice.compound != NULL ) {
            tagString = QString(_slice.compound->name.c_str());
        } else if (!_slice.srmId.empty()) {
            tagString = QString(_slice.srmId.c_str());
        }

        QString titleText =  tr("<b>%1</b> m/z: %2-%3").arg(
                                tagString,
                                QString::number(_slice.mzmin, 'f', 4),
                                QString::number(_slice.mzmax, 'f', 4)
        );

	QGraphicsTextItem* title = scene()->addText(titleText, font); 
	title->setHtml(titleText);
	int titleWith = title->boundingRect().width();
	title->setPos(scene()->width()/2-titleWith/2, 5);
	title->update();
}

void EicWidget::recompute(){
 //qDebug <<" EicWidget::recompute()";
    cleanup(); //more clean up
    computeEICs();	//retrive eics
    selectedGroup = NULL;

}

void EicWidget::wheelEvent(QWheelEvent *event) {
 //qDebug <<" EicWidget::wheelEvent(QWheelEvent *event)";

    if (_barplot != NULL && _barplot->isSelected() ) {
        QGraphicsView::wheelEvent(event);
    }

    if ( getSelectedGroup() ) {
        event->delta() > 0 ? _zoomFactor *=2 : _zoomFactor /=2;
        zoom(_zoomFactor);
        return;
    }

    float scale=1;
    event->delta() > 0 ? scale = 1.2 :  scale = 0.9;
    _slice.rtmin *=scale;
    _slice.rtmax /=scale;
    mzSlice bounds  = visibleSamplesBounds();
    if (_slice.rtmin > _slice.rtmax) swap(_slice.rtmin, _slice.rtmax);
    if (_slice.rtmin < bounds.rtmin) _slice.rtmin = bounds.rtmin;
    if (_slice.rtmax > bounds.rtmax) _slice.rtmax = bounds.rtmax;
    // qDebug() << "EicWidget::wheelEvent() " << _slice.rtmin << " " << _slice.rtmax << endl;
    replot(NULL);
}

void EicWidget::addFocusLine(PeakGroup* group) { 
 //qDebug <<" EicWidget::addFocusLine(PeakGroup* group)";
	//focus line
	if ( group == NULL ) return;

    if ( group->compound != NULL and group->compound->expectedRt > 0 ) {
		_focusLineRt=group->compound->expectedRt;
    }
    
    if (group->peaks.size() > 0 ) {
        Peak& selPeak = group->peaks[0]; 
        for(int i=1; i< group->peaks.size(); i++ ) {
            if ( group->peaks[i].peakIntensity > selPeak.peakIntensity ) {
                selPeak = group->peaks[i];
            }
        }
        Scan* scan = selPeak.getScan();

		if (getMainWindow()->spectraWidget && getMainWindow()->spectraWidget->isVisible() ) {
        	getMainWindow()->spectraWidget->setScan(scan, selPeak.peakMz-5, selPeak.peakMz+5);
		}
    }
	return;
}

void EicWidget::addAxes() { 
 //qDebug <<" EicWidget::addAxes()";
//	 qDebug() << "EicWidget: addAxes() " << _minY << " " << _maxY << endl;
    Axes* x = new Axes(0,_minX, _maxX,10);
    Axes* y = new Axes(1,_minY, _maxY,10);
    scene()->addItem(x);
    scene()->addItem(y);
	y->setOffset(20);
	y->showTicLines(true);
    x->setZValue(0);
    y->setZValue(0);
	return;
}

void EicWidget::addBarPlot(PeakGroup* group ) {
 //qDebug <<" EicWidget::addBarPlot(PeakGroup* group )";
	if ( group == NULL ) return;
	if (_barplot == NULL )  _barplot = new BarPlot(NULL,0);
	if (_barplot->scene() != scene() ) scene()->addItem(_barplot);

	_barplot->setMainWindow(getMainWindow());
	_barplot->setPeakGroup(group);

        int bwidth = _barplot->boundingRect().width();
        int bheight = _barplot->boundingRect().height();
	int xpos = scene()->width()*0.95-bwidth;
	int ypos = scene()->height()*0.10;
	_barplot->setPos(xpos,ypos);
        _barplot->setZValue(1000);

	float medianRt = group->medianRt();
	if ( group->parent ) medianRt = group->parent->medianRt();


	if (medianRt && group->parent) {
		float rt = toX(medianRt);
		float y1 = toY(_minY);
		float y2 = toY(_maxY);

		QPen pen2(Qt::blue, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                QGraphicsLineItem* focusLine = new QGraphicsLineItem(0,scene());
		focusLine->setPen(pen2);
		focusLine->setLine(rt, y1, rt, y2 );

		/*
		float x1 = toX(group->parent->minRt);
		float x2 = toX(group->parent->maxRt);
		QColor color = QColor::fromRgbF( 0.2, 0, 0 , 0.1 );
	  	QBrush brush(color); 
	   	QPen pen(color, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	 	scene()->addRect(QRectF(x1,y1,x2-x1,y2-y1),pen,brush);
		*/
	}
	return;
}

void EicWidget::addIsotopicPlot(PeakGroup* group) {
 //qDebug <<" EicWidget::addIsotopicPlot(PeakGroup* group)";
    if (group == NULL)  return;
    if (_isotopeplot == NULL) _isotopeplot = new IsotopePlot(0,scene());
    if (_isotopeplot->scene() != scene() ) scene()->addItem(_isotopeplot);
    _isotopeplot->hide();

    if (group->childCount() == 0) return;

    vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
    if (samples.size() == 0 ) return;

    _isotopeplot->setPos(scene()->width()*0.10,scene()->height()*0.10);
    _isotopeplot->setZValue(1000);
    _isotopeplot->setMainWindow(getMainWindow());
    _isotopeplot->setPeakGroup(group);
    _isotopeplot->show();
    return;
}

void EicWidget::addBoxPlot(PeakGroup* group) {
 //qDebug <<" EicWidget::addBoxPlot(PeakGroup* group)";
	if ( group == NULL ) return;
	if (_boxplot == NULL )  _boxplot = new BoxPlot(NULL,0);
	if (_boxplot->scene() != scene() ) scene()->addItem(_boxplot);

	_boxplot->setMainWindow(getMainWindow());
	_boxplot->setPeakGroup(group);

	int bwidth =  _boxplot->boundingRect().width();
        int bheight = _boxplot->boundingRect().height();

	int xpos = scene()->width()*0.95-bwidth;
	int ypos = scene()->height()*0.10;

	_boxplot->setPos(xpos,ypos);
        _boxplot->setZValue(1000);
	//_boxplot->setPos(scene()->width()*0.20,scene()->height()*0.10);
    //_boxplot->setZValue(1000);

	return;
}

void EicWidget::addFitLine(PeakGroup* group) {
 //qDebug <<"EicWidget::addFitLine(PeakGroup* group) ";
    if (group == NULL)  return;
    vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
	int steps=50;
	for(int i=0; i < group->peakCount(); i++ ) {
		Peak& p = group->peaks[i];
		if(p.hasEIC()==false) return;

		float rtWidth = p.rtmax-p.rtmin;
		float rtStep = rtWidth/steps;
		float s = 1.00;

		EIC* eic= p.getEIC();
		vector<mzPoint>observed = eic->getIntensityVector(p);
		if(observed.size() == 0 ) return;

		//find max point and total intensity
		float sum=0; 
		float maxpoint=0; float max=observed[0].y;
		for(int i=0; i < observed.size(); i++ ) { 
			sum += observed[i].y; 
			if (observed[i].y > max) { max=observed[i].y; maxpoint=i; }
		}
		if (sum == 0) return;
		/*
		//normalize
		for(int i=0; i < observed.size(); i++ ) { observed.y[i] /= sum; } 

		Line* line = new Line(0,scene());
		for(int i=0; i < observed.size(); i++ ) {
			line->addPoint(QPointF( toX(observed[i].x), toY(observed[i].y)));
		}

		QBrush brush(Qt::NoBrush);
		QPen   pen(Qt::black); pen.setWidth(3);
		line->setBrush(brush);
		line->setPen(pen);
		*/

		//samples
		vector<float>x(steps); 
		vector<float>y(steps);

		for(int i=0; i<steps; i++ ) {
			x[i]= p.rtmin + (float)i/steps*rtWidth;
			y[i]= mzUtils::pertPDF(x[i],p.rtmin,p.rt,p.rtmax)*p.peakIntensity/3;
			if(y[i] < 1e-3) y[i]=0;
			 qDebug() << x[i] << " " << y[i] << endl;
		}

		//for(int i=0; i < x.size(); i++ ) x[i]=p.rtmin+(i*rtStep);
                EicLine* line = new EicLine(0,scene());
		for(int i=0; i < y.size(); i++ ) {
			line->addPoint(QPointF( toX(x[i]), toY(y[i])));
		}

		QBrush brush(Qt::NoBrush);
		QPen   pen(Qt::black);
		line->setBrush(brush);
		line->setPen(pen);
                //line->fixEnds();
	}
    return;
}



void EicWidget::showAllPeaks() { 
 //qDebug <<"EicWidget::showAllPeaks() "; 
	for(int i=0; i < peakgroups.size(); i++ ) {
	PeakGroup& group = peakgroups[i];
        addPeakPositions(&group);
    }
}

void EicWidget::addPeakPositions(PeakGroup* group) {
 	////qDebug <<"EicWidget::addPeakPositions(PeakGroup* group) ";
		if ( _showPeaks == false ) return;

		bool setZValue=false;
		if (selectedGroup && group == selectedGroup ) {
				sort(group->peaks.begin(), group->peaks.end(), Peak::compIntensity); 
				setZValue=true;
		}

		for( unsigned int i=0; i < group->peaks.size(); i++) {
				Peak& peak = group->peaks[i];

				if ( peak.getSample() != NULL && peak.getSample()->isSelected == false ) continue;
				if ( _slice.rtmin != 0 && _slice.rtmax != 0 && (peak.rt < _slice.rtmin || peak.rt > _slice.rtmax) ) continue;

				QColor color = Qt::black;
				if (peak.getSample() != NULL ) {
						mzSample* s= peak.getSample();
						color = QColor::fromRgbF( s->color[0], s->color[1], s->color[2], s->color[3] );
				}

				QPen pen(color, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
				QBrush brush(color); brush.setStyle(Qt::NoBrush);

                                EicPoint* p  = new EicPoint(toX(peak.rt), toY(peak.peakIntensity), &peak, getMainWindow());
				if(setZValue) p->setZValue(i);
				p->setColor(color);
				p->setBrush(brush);
				p->setPen(pen);
				p->setPeakGroup(group);
				//connect(p,SIGNAL(addNote(Peak*)),this,SLOT(addNote(Peak*)));
				scene()->addItem(p);
		} 
}


void EicWidget::resetZoom() { 
 //qDebug <<"EicWidget::resetZoom() "; 
    mzSlice bounds(0,0,0,0);

    if( eics.size() > 0) {
        bounds = visibleEICBounds();
    } else if ( getMainWindow()->sampleCount() > 0 ) {
        vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
        bounds  = visibleSamplesBounds();
    }

    _slice.rtmin=bounds.rtmin;
    _slice.rtmax=bounds.rtmax;
    //qDebug() << "EicWidget::resetZoom() " << _slice.rtmin << " " << _slice.rtmax << endl;
    replot(NULL);
}

void EicWidget::zoom(float factor) {
 //qDebug <<"EicWidget::zoom(float factor) ";
    _zoomFactor = factor;
    if (_zoomFactor < 0.1 ) _zoomFactor = 0.1;
    if (_zoomFactor > 20 )  _zoomFactor = 20;

    if ( getSelectedGroup() ) {
        setPeakGroup(getSelectedGroup());
        replot(getSelectedGroup());
    }
}



MainWindow* EicWidget::getMainWindow() {  
// //qDebug <<"EicWidget::getMainWindow() ";  
	return (MainWindow*) parent; 
}

void EicWidget::setRtWindow(float rtmin, float rtmax ) {
 //qDebug <<"EicWidget::setRtWindow(float rtmin, float rtmax ) ";
	_slice.rtmin = rtmin;
	_slice.rtmax = rtmax;
}

void EicWidget::setSrmId(string srmId) { 
 //qDebug <<"EicWidget::setSrmId(string srmId) "; 
         //qDebug << "EicWidget::setSrmId" <<  srmId.c_str();
	_slice.compound = NULL;
	_slice.srmId = srmId;
   	recompute();
	resetZoom();
   	replot();
}

void EicWidget::setCompound(Compound* c ) {
    //qDebug << "EicWidget::setCompound()";
    //benchmark
//	timespec tS;
    //timespec tE;
    //clock_gettime(CLOCK_REALTIME, &tS);

//	for(int i =0; i < 100000; i++ ) { findPlotBounds(); }
    
    if ( c == NULL ) return;
    if ( getMainWindow()->sampleCount() == 0) return;

    vector <mzSample*> samples = getMainWindow()->getVisibleSamples();
    if (samples.size() == 0 ) return;

    int ionizationMode = samples[0]->getPolarity();
    if (getMainWindow()->getIonizationMode()) ionizationMode=getMainWindow()->getIonizationMode(); //user specified ionization mode

    float ppm = getMainWindow()->getUserPPM();
    float mz = 0;

    if (!c->formula.empty() ) {
        mz = c->ajustedMass(ionizationMode);
    } else {
        mz=c->mass;
    }

    //else {
    //    MassCalculator mcalc;
    //    mz =mcalc.adjustMass(c->mass,ionizationMode);
    //}

    float minmz = mz - mz/1e6*ppm;
    float maxmz = mz + mz/1e6*ppm;
    float rtmin = _slice.rtmin;
    float rtmax = _slice.rtmax;

    if (_autoZoom) {
        if (c->expectedRt > 0 ) {
            rtmin = c->expectedRt-2;
            rtmax = c->expectedRt+2;
        }
    }
    //clock_gettime(CLOCK_REALTIME, &tE);
   //qDebug() << "Time taken" << (tE.tv_sec-tS.tv_sec)*1000 + (tE.tv_nsec - tS.tv_nsec)/1e6;


    mzSlice slice(minmz,maxmz,rtmin,rtmax);
    slice.compound = c;
    if(!c->srmId.empty()) slice.srmId=c->srmId;
    setMzSlice(slice);

   //clock_gettime(CLOCK_REALTIME, &tE);
   //qDebug() << "Time taken" << (tE.tv_sec-tS.tv_sec)*1000 + (tE.tv_nsec - tS.tv_nsec)/1e6;


    for(int i=0; i < peakgroups.size(); i++ ) peakgroups[i].compound = c;
    if (c->expectedRt > 0 ) {
        setFocusLine(c->expectedRt);
        selectGroupNearRt(c->expectedRt);
    } 

//   clock_gettime(CLOCK_REALTIME, &tE);
  // qDebug() << "Time taken" << (tE.tv_sec-tS.tv_sec)*1000 + (tE.tv_nsec - tS.tv_nsec)/1e6;

}

void EicWidget::setMzSlice(const mzSlice& slice) {
    //qDebug << "EicWidget::setmzSlice()";
    if ( slice.mzmin != _slice.mzmin || slice.mzmax != _slice.mzmax  
         || slice.srmId != _slice.srmId
         || slice.compound != _slice.compound ) {
        _slice = slice;
        if ( slice.compound ) {
            if (slice.compound->precursorMz !=0 && slice.compound->productMz != 0 ) {
                _slice.mzmin = slice.compound->precursorMz;
                _slice.mzmax = slice.compound->productMz;
                _slice.mz    = slice.compound->precursorMz;
                _slice.compound=slice.compound;
                _slice.srmId  = slice.srmId;
            }
        }
        recompute();
    } else {
        _slice = slice;
    }
    replot(NULL);
}

void EicWidget::setMzRtWindow(float mzmin, float mzmax, float rtmin, float rtmax ) {
    //qDebug << "EicWidget::setMzRtWindow()";
    mzSlice slice(mzmin,mzmax,rtmin,rtmax);
	setMzSlice(slice);
}

void EicWidget::setPeakGroup(PeakGroup* group) {
 //qDebug <<"EicWidget::setPeakGroup(PeakGroup* group) ";

    if (group == NULL ) return;
    _slice.mz = group->meanMz;
    _slice.compound = group->compound;
    _slice.srmId = group->srmId;

    if (!group->srmId.empty()) {
        setSrmId(group->srmId);
    } else if ( group->compound ) {
        setCompound(group->compound);
    }

    if ( _autoZoom && group->parent != NULL ) {
        _slice.rtmin = group->parent->minRt- 2 * _zoomFactor;
        _slice.rtmax = group->parent->maxRt+ 2 * _zoomFactor;
    } else if (_autoZoom) {
        _slice.rtmin = group->minRt- 2 * _zoomFactor;
        _slice.rtmax = group->maxRt+ 2 * _zoomFactor;
    }

    //make sure that plot region is within visible samle bounds;
    mzSlice bounds = visibleEICBounds();
    if (_slice.rtmin < bounds.rtmin ) _slice.rtmin = bounds.rtmin;
    if (_slice.rtmax > bounds.rtmax ) _slice.rtmax = bounds.rtmax;

    _slice.mz = group->meanMz;
    if ( group->minMz != _slice.mzmin || group->maxMz != _slice.mzmax ) {
        _slice.mzmin = group->minMz;
        _slice.mzmax = group->maxMz;
        recompute();
    }

    if (group->compound)  for(int i=0; i < peakgroups.size(); i++ ) peakgroups[i].compound = group->compound;
    if (_slice.srmId.length())  for(int i=0; i < peakgroups.size(); i++ )  peakgroups[i].srmId = _slice.srmId;

    replot(group);
    addPeakPositions(group);
}



void EicWidget::setPPM(double ppm) { 
 //qDebug <<"EicWidget::setPPM(double ppm) "; 
        mzSlice x = _slice;
	if ( x.mz <= 0 ) x.mz = x.mzmin + (x.mzmax - x.mzmin)/2.0;
	x.mzmin = x.mz - x.mz/1e6*ppm;
	x.mzmax = x.mz + x.mz/1e6*ppm;
 	setMzSlice(x);
}

void EicWidget::setMzSlice(float mz){
     //qDebug() << "EicWidget::setMzSlice()" << setprecision(8) << mz << endl;
	mzSlice x (_slice.mzmin,_slice.mzmax,_slice.rtmin,_slice.rtmax);
	x.mz = mz;
	x.mzmin = mz - mz/1e6*getMainWindow()->getUserPPM();
	x.mzmax = mz + mz/1e6*getMainWindow()->getUserPPM();
	setMzSlice(x);
}

void EicWidget::groupPeaks() { 
	 //qDebug() << "EicWidget::groupPeaks() " << endl;
	//delete previous set of pointers to groups
	QSettings *settings 		= getMainWindow()->getSettings();
	float eic_smoothingWindow =   settings->value("eic_smoothingWindow").toDouble();
	float grouping_maxRtWindow =  settings->value("grouping_maxRtWindow").toDouble();

    peakgroups = EIC::groupPeaks(eics,eic_smoothingWindow,grouping_maxRtWindow);

	//keep only top X groups ( ranked by intensity )
	EIC::removeLowRankGroups(peakgroups,50);
}

void EicWidget::print(QPaintDevice* printer) {
 //qDebug <<"EicWidget::print(QPaintDevice* printer) ";
	QPainter painter(printer);

	if (! painter.begin(printer)) { // failed to open file
		qWarning("failed to open file, is it writable?");
		return;
	}
	render(&painter);
}

void EicWidget::addNote() {
 //qDebug <<"EicWidget::addNote() ";
	QPointF pos = _lastClickPos;
	float rt = invX(pos.x());
	float intensity = invY(pos.y());
	QString text;
	addNote(rt,intensity,text);
}

void EicWidget::addNote(Peak* peak) {
 //qDebug <<"EicWidget::addNote(Peak* peak) ";
	QString text;
	addNote(peak->rt,peak->peakIntensity,text);
}

void EicWidget::addNote(float rt, float intensity, QString text) { 
 //qDebug <<"EicWidget::addNote(float rt, float intensity, QString text) "; 

    if (text.isEmpty() ) {
        bool ok;
        text = QInputDialog::getText(this, tr("Add Note"), tr("Note:"), QLineEdit::Normal, "Your note", &ok);
        if ( !ok ) return;
    }

    QSettings* settings = getMainWindow()->getSettings();
    QString link = settings->value("data_server_url").toString();

    if (! text.isEmpty() && !link.isEmpty() ) {
        QUrl url(link);

        url.addQueryItem("action","addnote");

        if(_slice.compound) {
            if(!_slice.compound->name.empty())
                url.addQueryItem("compound_name", QString(_slice.compound->name.c_str()));

            if(!_slice.compound->id.empty())
                url.addQueryItem("compound_id", QString(_slice.compound->id.c_str()));
        }

        if(!_slice.srmId.empty()) url.addQueryItem("srm_id", QString(_slice.srmId.c_str()));
        url.addQueryItem("mz",  QString::number(_slice.mzmin+(_slice.mzmax-_slice.mzmin)/2,'f',6));
        url.addQueryItem("mzmin",  QString::number(_slice.mzmin,'f',6));
        url.addQueryItem("mzmax",  QString::number(_slice.mzmax,'f',6));
        url.addQueryItem("rt",  QString::number(rt,'f',6));
        url.addQueryItem("intensity",  QString::number(intensity,'f',2));
        url.addQueryItem("title",text);


        QDesktopServices::openUrl(url);

    }
    getNotes(_slice.mzmin,_slice.mzmax);
}


void EicWidget::updateNote(Note* note) { 
 //qDebug <<"EicWidget::updateNote(Note* note) "; 
	if (note == NULL) return;
	//getMainWindow()->notesDockWidgeth->updateNote(Note* note);
}

void EicWidget::getNotes(float mzmin, float mzmax) { 
 //qDebug <<"EicWidget::getNotes(float mzmin, float mzmax) "; 

        QSettings* settings = getMainWindow()->getSettings();

	if (getMainWindow()->notesDockWidget->isVisible() == false ) return;
	QList<UserNote*> notes = getMainWindow()->notesDockWidget->getNotes(mzmin,mzmax);
	foreach( UserNote* usernote, notes ) {

                float xpos = toX(usernote->rt);
                float ypos  = toY(usernote->intensity);
                if ( ypos < 10 ) ypos=10;
                if ( ypos > scene()->height()) ypos=scene()->height()+10;
                if ( xpos < 10 ) xpos=10;
                if ( xpos > scene()->width()) xpos=scene()->width()-10;

                Note* note = new Note(usernote);

                if ( settings->contains("data_server_url"))
                    note->setRemoteNoteLink(usernote,settings->value("data_server_url").toString());

                note->setStyle(Note::showNoteIcon);
                scene()->addItem(note);
                note->setPos(xpos,ypos);
	}
}

void EicWidget::contextMenuEvent(QContextMenuEvent * event) {
 //qDebug <<"EicWidget::contextMenuEvent(QContextMenuEvent * event) ";

    event->ignore();
    QMenu menu;
    QMenu options("Options");

    SettingsForm* settingsForm = getMainWindow()->settingsForm;

    QAction* d = menu.addAction("Peak Grouping Options");
    connect(d, SIGNAL(triggered()), settingsForm, SLOT(showPeakDetectionTab()));
    connect(d, SIGNAL(triggered()), settingsForm, SLOT(show()));

    QAction* a = menu.addAction("Add a Note");
    connect(a, SIGNAL(triggered()), SLOT(addNote()));

    QAction* b = menu.addAction("Recalculate EICs");
    connect(b, SIGNAL(triggered()), SLOT(replotForced()));

    QAction* c = menu.addAction("Copy EIC(s) to Clipboard");
    connect(c, SIGNAL(triggered()), SLOT(eicToClipboard()));

    menu.addMenu(&options);

    QAction* o4 = options.addAction("Show Peaks");
    o4->setCheckable(true);
    o4->setChecked(_showPeaks);
    connect(o4, SIGNAL(toggled(bool)), SLOT(showPeaks(bool)));
    connect(o4, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction* o1 = options.addAction("Show Spline");
    o1->setCheckable(true);
    o1->setChecked(_showSpline);
    connect(o1, SIGNAL(toggled(bool)), SLOT(showSpline(bool)));
    connect(o1, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction* o2 = options.addAction("Show Baseline");
    o2->setCheckable(true);
    o2->setChecked(_showBaseline);
    connect(o2, SIGNAL(toggled(bool)), SLOT(showBaseLine(bool)));
    connect(o2, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction* o3 = options.addAction("Show TIC");
    o3->setCheckable(true);
    o3->setChecked(_showTicLine);
    connect(o3, SIGNAL(toggled(bool)), SLOT(showTicLine(bool)));
    connect(o3, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction* o5 = options.addAction("Show Bar Plot");
    o5->setCheckable(true);
    o5->setChecked(_showBarPlot);
    connect(o5, SIGNAL(toggled(bool)), SLOT(showBarPlot(bool)));
    connect(o5, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction* o6 = options.addAction("Show Isotope Plot");
    o6->setCheckable(true);
    o6->setChecked(_showIsotopePlot);
    connect(o6, SIGNAL(toggled(bool)), SLOT(showIsotopePlot(bool)));
    connect(o6, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction* o7 = options.addAction("Show Box Plot");
    o7->setCheckable(true);
    o7->setChecked(_showBoxPlot);
    connect(o7, SIGNAL(toggled(bool)), SLOT(showBoxPlot(bool)));
    connect(o7, SIGNAL(toggled(bool)), SLOT(replot()));

    QAction *selectedAction = menu.exec(event->globalPos());
    scene()->update();

}

void EicWidget::eicToClipboard() { 
 //qDebug <<"EicWidget::eicToClipboard() "; 

	if (eics.size() == 0 ) return;
	QString eicText;
	for(int i=0; i < eics.size(); i++ ) {
		EIC* e = eics[i]; 
		if (e == NULL ) continue;
		mzSample* s = e->getSample();
		if (s == NULL ) continue;

		eicText += QString(s->sampleName.c_str()) + "\n";
				 
		for(int j=0;  j<e->size(); j++ ) {
				if (e->rt[j] >= _slice.rtmin && e->rt[j] <= _slice.rtmax ) {
						eicText += tr("%1,%2,%3,%4\n").arg( 
								QString::number(i),
								QString::number(e->rt[j], 'f', 2),
								QString::number(e->intensity[j], 'f', 4),
								QString::number(e->mz[j], 'f', 3)
						);
				}
		}
	}

	QClipboard *clipboard = QApplication::clipboard();
	clipboard->setText(  eicText );

}


void EicWidget::selectGroupNearRt(float rt) {
 //qDebug <<"EicWidget::selectGroupNearRt(float rt) ";
	float minDiff = FLT_MAX;
	if ( peakgroups.size() == 0 ) return;

    PeakGroup* selGroup = NULL;

	for(int i=0; i < peakgroups.size(); i++ ) {
       float diff = abs(peakgroups[i].meanRt - rt);
	   if ( diff < 2 ) { 
			if (selGroup == NULL ) { selGroup = &peakgroups[i]; continue; }
			if (selGroup != NULL && peakgroups[i].maxIntensity > selGroup->maxIntensity ) {
			   	selGroup = &peakgroups[i];
			}
		}
	}

	if( selGroup ) {
		setSelectedGroup(selGroup);
	}
}

void EicWidget::setSelectedGroup(PeakGroup* group ) {
 //qDebug <<"EicWidget::setSelectedGroup(PeakGroup* group ) ";
	if (_frozen || group == NULL) return;
	if (_showBarPlot)     addBarPlot(group);
	if (_showIsotopePlot) addIsotopicPlot(group);
	if (_showBoxPlot)     addBoxPlot(group);
	drawSelectionLine(group->minRt, group->maxRt);
	//addFitLine(group);
	selectedGroup = group;
}

void EicWidget::setGallaryToEics() {

    if(getMainWindow()->galleryDockWidget->isVisible()) {
        getMainWindow()->galleryWidget->addIdividualEicPlots(eics,selectedGroup);
    }
}

void EicWidget::saveRetentionTime() {
 //qDebug <<"EicWidget::saveRetentionTime() ";
	if (!selectedGroup || selectedGroup->compound == NULL ) return;

	QPointF pos = _lastClickPos;
	float rt = invX(pos.x());
	selectedGroup->compound->expectedRt = rt;

	DB.saveRetentionTime(selectedGroup->compound, rt, "user_method");
}

void EicWidget::align() { 
 //qDebug <<"EicWidget::align() "; 
	if (peakgroups.size() == 0) return;
	vector<PeakGroup*>_groups;
	for(int i=0; i < peakgroups.size(); i++ ) {
		if (peakgroups[i].goodPeakCount > 1 ) _groups.push_back(&peakgroups[i]);
	}
	Aligner aligner;
	MainWindow* mainwindow = getMainWindow();
	aligner.setMaxItterations(mainwindow->alignmentDialog->maxItterations->value());
	aligner.setPolymialDegree(mainwindow->alignmentDialog->polynomialDegree->value());
	aligner.doAlignment(_groups);
	recompute();
	replot();
}


void EicWidget::keyPressEvent( QKeyEvent *e ) {
 //qDebug <<"EicWidget::keyPressEvent( QKeyEvent *e ) ";

    QGraphicsView::keyPressEvent(e);

    switch( e->key() ) {
    case Qt::Key_Down:
        getMainWindow()->ligandWidget->showNext();
        break;
    case Qt::Key_Up:
        getMainWindow()->ligandWidget->showLast();
        break;
    case Qt::Key_Left:
        getMainWindow()->spectraWidget->showLastScan();
        break;
    case Qt::Key_Right :
        getMainWindow()->spectraWidget->showNextScan();
        break;
    case Qt::Key_0 :
        resetZoom();
        break;
    case Qt::Key_Plus :
        zoom(_zoomFactor * 0.9 );
        break;
    case Qt::Key_Minus :
        zoom(_zoomFactor * 1.1 );
        break;
    case Qt::Key_C :
        copyToClipboard();
        break;
    case Qt::Key_G:
        markGroupGood();
        break;
    case Qt::Key_B:
        markGroupBad();
        break;
    case Qt::Key_F5:
        replotForced();

    case Qt::Key_I:
        toggleAreaIntegration(true);
        break;

    case Qt::Key_A:
        toggleSpectraAveraging(true);
        break;


    default:
        break;
    }
    e->accept();
    return;
}

void EicWidget::setStatusText(QString text) {
 //qDebug <<"EicWidget::setStatusText(QString text) ";
	if (_statusText == NULL) { 
		_statusText = new Note(text,0,scene()); 
		_statusText->setExpanded(true); 
		_statusText->setFlag(QGraphicsItem::ItemIsSelectable,false);
                _statusText->setFlag(QGraphicsItem::ItemIsFocusable,false);
		_statusText->setFlag(QGraphicsItem::ItemIsMovable,false);
                _statusText->setZValue(2000);
            }

	if (_statusText->scene() != scene() ) scene()->addItem(_statusText);
	_statusText->setHtml(text);
        _statusText->setTimeoutTime(10);

        QRectF size = _statusText->boundingRect();
        _statusText->setPos(scene()->width()-size.width()-5,scene()->height()-size.height()-5);

}


void EicWidget::markGroupGood()  { getMainWindow()->markGroup(getSelectedGroup(),'g'); }
void EicWidget::markGroupBad()   { getMainWindow()->markGroup(getSelectedGroup(),'b'); }
void EicWidget::copyToClipboard(){ getMainWindow()->setClipboardToGroup(getSelectedGroup()); }

void EicWidget::freezeView(bool freeze) {
 //qDebug <<"EicWidget::freezeView(bool freeze) ";
    if (freeze == true) {
        _frozen=true;
        _freezeTime=100;
        _timerId=startTimer(50);
    } else {
        _frozen=false;
        _freezeTime=0;
        killTimer(_timerId);
        _timerId=0;
    }
}

void EicWidget::timerEvent( QTimerEvent * event ) {
 //qDebug <<"EicWidget::timerEvent( QTimerEvent * event ) ";
    _freezeTime--;
    if(_freezeTime<=0) { killTimer(_timerId); _frozen=false; _freezeTime=0; _timerId=0; }
}