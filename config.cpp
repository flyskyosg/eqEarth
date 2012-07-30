#include "config.h"

#include "error.h"
#include "util.h"
#include "vps.h"
#include "callbacks.h"
#include "window.h" // for Window::initCapabilities
#include "configEvent.h"

#include "earthManipulator.h"
#include <osgEarth/TerrainEngineNode>
#include <osgGA/TrackballManipulator>

#include <osg/DeleteHandler>
#include <osg/ImageStream>
#include <osg/TextureRectangle>
#include <osgDB/ReadFile>

#include <osgEarth/FindNode>

//#include <GL/glu.h>

#include <osgEarthUtil/ObjectPlacer>

#include <osgEarthDrivers/kml/KML>
#include <osgEarthDrivers/ocean_surface/OceanSurface>
#include <osgEarth/ShaderComposition>

#define NFR_AT_RADIUS 0.00001
#define NFR_AT_DOUBLE_RADIUS 0.0049

namespace eqEarth
{
// ----------------------------------------------------------------------------

struct NullGraphicsContext : public osgViewer::GraphicsWindow
{
NullGraphicsContext( )
{
    _traits = new GraphicsContext::Traits;
    _traits->x = 0;
    _traits->y = 0;
    _traits->width = 0;
    _traits->height = 0;
}

void setPVP( uint32_t width, uint32_t height )
{
     _traits->width = width;
     _traits->height = height;
}

void clearCameras( ) { _cameras.clear( ); }
};

static osg::ref_ptr< NullGraphicsContext > ngc =
    new NullGraphicsContext;

// ----------------------------------------------------------------------------

struct Config::ViewCollector : public eq::ConfigVisitor
{
ViewCollector( Config* config )
    : _config( config )
{
}

virtual eq::VisitorResult visit( eq::View* view )
{
    View* v = static_cast< View* >( view );
    osgViewer::View* osgView;

    v->setSceneID( 1 );

    osgView = _config->takeOrCreateOSGView( v->getSceneID( ));

    v->setOSGView( osgView );

    if( !osgView->getCameraManipulator( ))
    {
        osgGA::CameraManipulator* m;

        osgEarth::MapNode* map =
            osgEarth::MapNode::findMapNode( osgView->getSceneData( ));
        if( map )
        {
            m = new EarthManipulator;
            m->setNode( map->getTerrainEngine( ));
        }
        else
        {
            m = new osgGA::TrackballManipulator;
            m->setHomePosition(
                osg::Vec3d( 0, 0, 10 ),
                osg::Vec3d( 0, 0, 0 ),
                osg::Vec3d( 0, 1, 0 ), false );
            m->setNode( osgView->getSceneData( ));
        }

        osgView->setCameraManipulator( m );
    }

    _config->releaseOSGView( osgView );

    LBCHECK( _config->selectCurrentView( v->getID( )));

    return eq::TRAVERSE_CONTINUE;
}

public:
    Config* _config;
};

// ----------------------------------------------------------------------------

struct ViewUpdater : public eq::ConfigVisitor
{
virtual eq::VisitorResult visit( eq::View* view )
{
    View* v = static_cast< View* >( view );
    osgViewer::View* osgView = v->getOSGView( );
    LBASSERT( osgView );
    const osgGA::CameraManipulator* m = osgView->getCameraManipulator( );
    LBASSERT( m );
    const osg::Matrixd& viewMatrix = m->getInverseMatrix( );

    /* NEAR/FAR */
    osgEarth::MapNode* map = osgEarth::MapNode::findMapNode(
        osgView->getSceneData( ));
    if( map && map->isGeocentric( ))
    {
        osg::Vec3d eye, center, up;
        viewMatrix.getLookAt( eye, center, up );
        double d = eye.length( );

        double rp = map->getMap( )->
            getProfile( )->getSRS( )->getEllipsoid()->getRadiusPolar( );

        if( d > rp )
        {
            double zf = ::sqrt( d * d - rp * rp );
            double nfr = NFR_AT_RADIUS + NFR_AT_DOUBLE_RADIUS *
                (( d - rp ) / d );
            double zn = osg::clampAbove( zf * nfr, 1.0 );

            v->setNearFar( zn, zf );
        }
    }

    /* VIEW MATRIX */
    v->setViewMatrix( osgToVmml( viewMatrix ));

    return eq::TRAVERSE_CONTINUE;
}
};

// ----------------------------------------------------------------------------

Config::Config( eq::ServerPtr parent )
    : eq::Config( parent )
    , _appRenderTick( 0U )
{
LBINFO << "=====> Config::Config(" << (void *)this << ")" << std::endl;

    //srandom( time( NULL ));
    srandom( 1 );

    osg::Referenced::setThreadSafeReferenceCounting( true );

    //osg::Referenced::setDeleteHandler( new osg::DeleteHandler( 2 ));

    // initialize the global timer to be relative to the current time.
    osg::Timer::instance( )->setStartTick( );

    osg::ref_ptr< osg::DisplaySettings > ds =
        osg::DisplaySettings::instance( );
    ds->setMaxTexturePoolSize( 100000000 );      // size in bytes (~100Mb)
    ds->setMaxBufferObjectPoolSize( 200000000 ); // size in bytes (~200Mb)
    ds->setSerializeDrawDispatch( false  );
    ds->setCompileContextsHint( false  );
#if 1
    ds->setNumOfDatabaseThreadsHint( 2 );
    ds->setNumOfHttpDatabaseThreadsHint( 2 );
#endif

    _ico = new osgUtil::IncrementalCompileOperation( );
    _ico->setTargetFrameRate( 60.0f );

    _pager = osgDB::DatabasePager::create( );
    _pager->setUnrefImageDataAfterApplyPolicy( false, false );
    if( _ico.valid( ))
        _pager->setIncrementalCompileOperation( _ico );
}

Config::~Config( )
{
LBINFO << "<===== Config::~Config(" << (void *)this << ")" << std::endl;
}

bool Config::init( )
{
LBINFO << "-----> Config::init( )" << std::endl;

    registerObject( &_frameData );
    _initData.setFrameDataID( _frameData.getID( ));
    registerObject( &_initData );

    bool init = false;

    _viewer = new CompositeViewer;
    _viewer->setThreadingModel( osgViewer::ViewerBase::ThreadPerCamera );
    _viewer->setStartTick( osg::Timer::instance( )->getStartTick( ));
    if( _ico.valid( ))
        _viewer->setIncrementalCompileOperation( _ico );

    if( !eq::Config::init( _initData.getID( )))
        goto out;

    {
        ViewCollector m( this );
        accept( m );
    }

    init = true;

out:
    if( !init )
        cleanup( );

LBINFO << "<----- Config::init( )" << std::endl;

    return init;
}

bool Config::exit( )
{
    bool ret = eq::Config::exit( );

    cleanup( );

    return ret;
}

uint32_t Config::startFrame( )
{
    ViewUpdater m;
    accept( m );

    const double t = static_cast< double >( getTime( )) / 1000.;
    _frameData.setSimulationTime( t );
    _frameData.setCalendarTime( time( NULL ));

    uint32_t ret = eq::Config::startFrame( _frameData.commit( ));

    if(( _viewer->getNumViews( ) > 0 ) && ( ++_appRenderTick > 1 ))
    {
        if( !_gc.valid( ) && !appInitGL( ))
            LBWARN << "Unable to create application GL context" << std::endl;

        if( _gc.valid( ))
        {
            //LBWARN << "app render frame #" << getCurrentFrame( ) << std::endl;

            _viewer->setGlobalContext( _gc );

            _viewer->frameStart( getCurrentFrame( ), _frameData, false );

            _viewer->renderingTraversals( );
        }
    }

    return ret;
}

uint32_t Config::finishFrame( )
{
    if( _viewer->getNumViews( ) > 0 )
    {
        if( _gc.valid( ))
            _viewer->frameDrawFinish( false );
    }
    else
        _appRenderTick = 0U;

    return eq::Config::finishFrame( );
}

bool Config::mapInitData( const eq::uint128_t& initDataID )
{
    bool mapped = false;
    if( !_initData.isAttached( ))
    {
        const uint32_t request = mapObjectNB( &_initData, initDataID,
            co::VERSION_OLDEST, getApplicationNode( ));
        if( mapObjectSync( request ))
        {
            unmapObject( &_initData );
            mapped = true;
        }
    }
    else
    {
        LBASSERT( _initData.getID( ) == initDataID );
        mapped = true;
    }
    return mapped;
}

bool Config::handleEvent( const eq::ConfigEvent* event )
{
    bool ret = false;
    const double time = static_cast< double >( getTime( )) / 1000.;

    switch( event->data.type )
    {
        case eq::Event::WINDOW_POINTER_WHEEL:
        case eq::Event::CHANNEL_POINTER_MOTION:
        case eq::Event::CHANNEL_POINTER_BUTTON_PRESS:
        case eq::Event::CHANNEL_POINTER_BUTTON_RELEASE:
        {
            View* view =
                selectCurrentView( event->data.context.view.identifier );
            if( view && _eventQueue.valid( ))
            {
                handleMouseEvent( event, view, time );
                ret = true;
            }
            break;
        }
        case eq::Event::KEY_PRESS:
        {
            const int osgKey = eqKeyToOsg( event->data.key.key );
            if( _eventQueue.valid( ))
                _eventQueue->keyPress( osgKey, time );

            if( 's' == event->data.key.key )
            {
                _frameData.toggleStatistics( );
                ret = true;
            }

            if( 't' == event->data.key.key )
            {
                const eq::uint128_t& currentViewID =
                    _frameData.getCurrentViewID( );
                View* view =
                    static_cast< View* >( find< eq::View >( currentViewID ));
                if( view )
                {
                    const eq::View::Mode mode = view->getMode( );
                    if( eq::View::MODE_MONO == mode )
                        view->changeMode( eq::View::MODE_STEREO );
                    else
                        view->changeMode( eq::View::MODE_MONO );
                }
                ret = true;
            }

            if( '1' == event->data.key.key )
            {
                eq::Canvas* canvas = find< eq::Canvas >( "clove" );
                if( canvas )
                {
                    int64_t index = canvas->getActiveLayoutIndex( );
                    canvas->useLayout( index ? 0 : 1 );
                }
                ret = true;
            }
            break;
        }
        case eq::Event::KEY_RELEASE:
        {
            const int osgKey = eqKeyToOsg( event->data.key.key );
            if( _eventQueue.valid( ))
                _eventQueue->keyRelease( osgKey, time );
            break;
        }
        case ConfigEvent::INTERSECTION:
        {
            const ConfigEvent* hitEvent =
                static_cast< const ConfigEvent* >( event );

            LBINFO << std::fixed << hitEvent << " from " <<
                hitEvent->data.originator << std::endl;
#if 0
if( _map.valid( ))
{

        double lat_rad, lon_rad, height;
        _map->getMap( )->getProfile( )->getSRS( )->getEllipsoid(
)->convertXYZToLatLongHeight( hitEvent->hit.x( ), hitEvent->hit.y( ),
hitEvent->hit.z( ), lat_rad, lon_rad, height );

        // query the elevation at the map point:
        double lat_deg = osg::RadiansToDegrees( lat_rad );
        double lon_deg = osg::RadiansToDegrees( lon_rad );

        LBWARN << "\tHIT(" << lat_deg << ", " << lon_deg << ")" << std::endl;
}
#endif
            ret = true;
            break;
        }
    }

    if( !ret )
        ret = eq::Config::handleEvent( event );

    return ret;
}

osgViewer::View* Config::takeOrCreateOSGView( const eq::uint128_t& sceneID )
{
    const bool needViewerLock = ( getNodes( ).size( ) > 1 );
    lunchbox::ScopedWrite _mutex( needViewerLock ? &_viewer_lock : 0 );

    osgViewer::View* osgView = NULL; // do *not* use ref_ptr here

    if( _viewer.valid( )) // i.e. isApplicationNode
    {
        osgView = _viewer->findOSGViewByID( sceneID );
        if( osgView )
        {
            _viewer->removeView( osgView ); // eqEarth::View still has a ref

            osgView->getCamera( )->setGraphicsContext( 0 );
            osgView->getCamera( )->setViewport( 0 );
        }
    }

    if( !osgView )
    {
        osgView = CompositeViewer::createOSGView( sceneID );

        osgView->getCamera( )->setComputeNearFarMode(
            osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR );

        osgView->setSceneData( getScene( sceneID ));
        osgView->setDatabasePager( _pager );

        osgEarth::Util::SkyNode* sky =
            osgEarth::findTopMostNodeOfType< osgEarth::Util::SkyNode >(
                osgView->getSceneData( ));
        if( sky )
            sky->attach( osgView );
    }

    return osgView;
}

void Config::releaseOSGView( osgViewer::View* osgView )
{
    const bool needViewerLock = ( getNodes( ).size( ) > 1 );
    lunchbox::ScopedWrite _mutex( needViewerLock ? &_viewer_lock : 0 );

    if( _viewer.valid( )) // i.e. isApplicationNode
    {
        osgView->getCamera( )->setGraphicsContext( 0 );
        osgView->getCamera( )->setViewport( 0 );

        _viewer->addView( osgView );
    }
}

osg::Group* Config::getScene( const eq::uint128_t& sceneID )
{
    LBASSERT( sceneID == 1 ); // TODO : multiple scenes

    if( !_scene.valid( ))
    {
        osg::Group* group = new osg::Group( );

        group->addChild( osgDB::readNodeFile( _initData.getModelFileName( )));

        osgEarth::MapNode* map = osgEarth::MapNode::findMapNode( group );
        if( map && map->getMap( )->getProfile( ) &&
            map->getMap()->isGeocentric( ))
        {
            osgEarth::Util::SkyNode* sky =
                new osgEarth::Util::SkyNode( map->getMap( ));

            sky->addUpdateCallback( new SkyUpdateCallback );

            group->addChild( sky );

#if 0
            osgEarth::Drivers::OceanSurfaceNode* ocean =
                new osgEarth::Drivers::OceanSurfaceNode( map,
                    map->externalConfig( ).child( "ocean" ));

            group->addChild( ocean );
#endif

            osgEarth::Util::ObjectPlacer o( map );

/*
            {
               osg::ref_ptr< osg::Image > i = osgDB::readImageFile( "134.207.0.141.gst" );
               if( i.valid( ))
               {
                  osg::ref_ptr< osg::ImageStream > is = dynamic_cast< osg::ImageStream* >( i.get( ));

                  osg::Vec3 pos(0.0f,0.0f,0.0f);
                  osg::ref_ptr<osg::Drawable> drawable = myCreateTexturedQuadGeometry(pos, 640, 480, i, true, false, true);

                  osg::ref_ptr<osg::Geode> geode = new osg::Geode;

                  osg::StateSet* stateset = geode->getOrCreateStateSet();
                  stateset->setMode(GL_LIGHTING,osg::StateAttribute::OFF);
                  geode->addDrawable(drawable.get());

                  group->addChild( o.placeNode( geode, 39.997, -74.6157, 10 ));
               }
            }
*/

/*
            {
               osg::ref_ptr< osg::Node > n = osgDB::readNodeFile(
                  "/lustre/site_x/data2/models/FtDix/archive.txp" );
               if( n.valid( ))
                  group->addChild( o.placeNode( n, 39.997, -74.6157, 5 ));
            }

            {
               osg::ref_ptr< osg::Node > n = osgDB::readNodeFile(
                  "/lustre/site_x/data2/models/FtOrdMOUT/Flythough/archive.txp" );
               if( n.valid( ))
                  group->addChild( o.placeNode( n, 36.5824, -121.8091, 100 ));
            }

*/
/*
            {
                osgEarth::Drivers::KMLOptions kmlo;
                kmlo.declutter( ) = true;
                kmlo.defaultIconImage( ) = osgEarth::Util::URI( "http://www.osgearth.org/chrome/site/pushpin_yellow.png" ).getImage( );

               osg::ref_ptr< osg::Node > kml = osgEarth::Drivers::KML::load( osgEarth::Util::URI( "/lustre/cmf/users/d/dkleiner/forks/osgearth/data/BostonBldgs.kmz" ), map, kmlo );
               if( kml.valid( ))
                  group->addChild( kml );
            }
*/

        }

#if 0
        {
            char s_source[] =
                "void osgearth_frag_applyColoring( inout vec4 color ) { \n"
                "    color = vec4(1.0, 0.0, 0.0, 1.0); \n"
                "} \n";

            osgEarth::VirtualProgram* vp = new osgEarth::VirtualProgram();
            vp->setShader( "osgearth_frag_applyColoring",
                new osg::Shader(osg::Shader::FRAGMENT, s_source),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE );
            group->getOrCreateStateSet()->setAttributeAndModes( vp,
                osg::StateAttribute::ON );
        }
#endif

        _scene = group;
    }
    return _scene.get( );
}

void Config::cleanup( )
{
    deregisterObject( &_initData );
    deregisterObject( &_frameData );

    _initData.setFrameDataID( eq::UUID::ZERO );

    _eventQueue = 0;
    _viewer = 0;

    // Don't deref _scene until _pager threads are finished
    _pager = 0;
    _ico = 0;

    if( osg::Referenced::getDeleteHandler( ))
    {
        osg::Referenced::getDeleteHandler( )->setNumFramesToRetainObjects( 0 );
        osg::Referenced::getDeleteHandler( )->flushAll( );
    }

    _scene = 0;

    _gc = 0;
}

View* Config::selectCurrentView( const eq::uint128_t& viewID )
{
    View* view = static_cast< View* >( find< eq::View >( viewID ));
    if( view )
    {
        _frameData.setCurrentViewID( viewID );
        _eventQueue = view->getOSGView( )->getEventQueue( );
    }
    return view;
}

void Config::handleMouseEvent( const eq::ConfigEvent* event, View* view,
        double time )
{
    const eq::PixelViewport& pvp = event->data.context.pvp;
    const uint32_t x = event->data.pointer.x;
    const uint32_t y = event->data.pointer.y;

    LBASSERT( _eventQueue.valid( ));

    switch( event->data.type )
    {
        case eq::Event::WINDOW_POINTER_WHEEL:
        {
            osgGA::GUIEventAdapter::ScrollingMotion sm =
                osgGA::GUIEventAdapter::SCROLL_NONE;
            if( event->data.pointer.xAxis > 0 )
                sm = osgGA::GUIEventAdapter::SCROLL_UP;
            else if( event->data.pointer.xAxis < 0 )
                sm = osgGA::GUIEventAdapter::SCROLL_DOWN;
            else if( event->data.pointer.yAxis > 0 )
                sm = osgGA::GUIEventAdapter::SCROLL_RIGHT;
            else if( event->data.pointer.yAxis < 0 )
                sm = osgGA::GUIEventAdapter::SCROLL_LEFT;
            _eventQueue->mouseScroll( sm, time );
            break;
        }
        case eq::Event::CHANNEL_POINTER_MOTION:
            _eventQueue->setMouseInputRange( 0, 0, pvp.w, pvp.h );
            _eventQueue->mouseMotion( x, y, time );
            break;
        case eq::Event::CHANNEL_POINTER_BUTTON_PRESS:
        {
            const unsigned int b = eqButtonToOsg( event->data.pointer.button );
            if( b <= 3 )
            {
                _eventQueue->setMouseInputRange( 0, 0, pvp.w, pvp.h );
                _eventQueue->mouseButtonPress( x, y, b, time );
            }
            break;
        }
        case eq::Event::CHANNEL_POINTER_BUTTON_RELEASE:
        {
            const unsigned int b = eqButtonToOsg( event->data.pointer.button );
            if( b <= 3 )
            {
                _eventQueue->setMouseInputRange( 0, 0, pvp.w, pvp.h );
                _eventQueue->mouseButtonRelease( x, y, b, time );
            }
            break;
        }
        default:
            break;
    }

    osgGA::EventQueue::Events events;
    _eventQueue->takeEvents( events );

    for( osgGA::EventQueue::Events::iterator itr = events.begin( );
        itr != events.end( ); ++itr)
    {
        osgViewer::View* osgView = view->getOSGView( );
        LBASSERT( osgView );

        osg::ref_ptr< osgGA::CameraManipulator > m =
            osgView->getCameraManipulator( );
        if( m.valid( ))
        {
            osg::ref_ptr< osg::Camera > camera = osgView->getCamera( );

            ngc->setPVP( pvp.w, pvp.h );

            camera->setGraphicsContext( ngc );

            // viewport
            camera->setViewport( 0, 0, pvp.w, pvp.h );

            // perspective
            double near, far;
            eq::Frustumf frustum = event->data.context.frustum;
            view->getNearFar( near, far );
            frustum.adjust_near( near );
            frustum.far_plane( ) = far;
            camera->setProjectionMatrixAsFrustum(
                frustum.left( ), frustum.right( ),
                frustum.bottom( ), frustum.top( ),
                frustum.near_plane( ), frustum.far_plane( ));

            // perspective transform
            const eq::Matrix4d& viewMatrix = view->getViewMatrix( );
            const eq::Matrix4d& headTransform =
                event->data.context.headTransform;
            camera->setViewMatrix( vmmlToOsg( headTransform * viewMatrix));

            m->handleWithCheckAgainstIgnoreHandledEventsMask(
                *itr->get( ), *osgView);

            ngc->clearCameras( );

            camera->setGraphicsContext( 0 );
            camera->setViewport( 0 );
        }
    }
}

void Config::updateCurrentWorldPointer( const eq::ConfigEvent* event )
{
#if 0
    const eq::PixelViewport& pvp = event->data.context.pvp;

    float x = static_cast< float >( event->data.pointer.x ) + pvp.x;
    float y = pvp.h - static_cast< float >( event->data.pointer.y ) + pvp.y;

#if 0
LBWARN << "xy: " << x << ", " << y << " in " << pvp << std::endl;
#endif

    const eq::Matrix4d viewMatrix =
        eq::Matrix4d( event->data.context.headTransform ) *
            _frameData.getViewMatrix( );

    const eq::Frustumf& frustum = event->data.context.frustum;
    const eq::Matrix4d projectionMatrix = frustum.compute_matrix( );

    eq::Vector3d p1, p2;
    LBCHECK(
        gluUnProject( x, y, 0, // near plane
            viewMatrix.array, projectionMatrix.array, &pvp.x,
            &p1.x( ), &p1.y( ), &p1.z( ))
        &&
        gluUnProject( x, y, 1, // far plane
            viewMatrix.array, projectionMatrix.array, &pvp.x,
            &p2.x( ), &p2.y( ), &p2.z( )));

    const eq::uint128_t& viewID = event->data.context.view.identifier;
    View* view = static_cast< View* >( find< eq::View >( viewID ));
    LBASSERT( NULL != view );
    view->setWorldPointer( p1, p2 );
#endif
}

bool Config::appInitGL( bool pbuffer )
{
    osg::ref_ptr< osg::DisplaySettings > ds =
        osg::DisplaySettings::instance( );

    osg::ref_ptr< osg::GraphicsContext::Traits > traits =
        new osg::GraphicsContext::Traits( ds );

    traits->readDISPLAY( );
    if( traits->displayNum < 0 ) traits->displayNum = 0;
    if( traits->screenNum < 0 ) traits->screenNum = 0;

    traits->x = 0;
    traits->y = 0;
    traits->width = 10;
    traits->height = 10;
    traits->windowDecoration = false;
    traits->doubleBuffer = false;
    traits->sharedContext = 0;
    traits->pbuffer = pbuffer;

    _gc = osg::GraphicsContext::createGraphicsContext( traits );

    if( !_gc.valid( ))
        return false;

    LBCHECK( _gc->realize( ));

    _gc->makeCurrent( );

    if( _ico.valid( ))
        _ico->addGraphicsContext( _gc );

    const unsigned int maxTexturePoolSize =
        osg::DisplaySettings::instance( )->getMaxTexturePoolSize( );
    const unsigned int maxBufferObjectPoolSize =
        osg::DisplaySettings::instance( )->getMaxBufferObjectPoolSize( );

    if( maxTexturePoolSize > 0 )
        _gc->getState( )->setMaxTexturePoolSize( maxTexturePoolSize );
    if( maxBufferObjectPoolSize > 0 )
        _gc->getState( )->setMaxBufferObjectPoolSize(
            maxBufferObjectPoolSize );

    Window::initCapabilities( _gc );

    return true;
}
}
