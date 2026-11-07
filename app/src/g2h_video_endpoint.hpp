#pragma once
#include <app/util/attribute-storage.h>
#include <app-common/zap-generated/ids/Clusters.h>
namespace g2h {
// Vendor development utility endpoint for the capture control. It is not a
// plug, light, camera bridge or second camera. The control is standard Mode
// Select so existing controllers can encode its command without a vendor model.
class VideoEndpoint {
public:
 CHIP_ERROR Register(){
  if(active_)return CHIP_ERROR_INCORRECT_STATE;
  const auto err=emberAfSetDynamicEndpoint(0,2,&endpoint_,chip::Span(versions_),chip::Span<const EmberAfDeviceType>(types_),1);
  active_=err==CHIP_NO_ERROR;return err;
 }
 void Clear(){if(active_){emberAfClearDynamicEndpoint(0);active_=false;}}
private:
 inline static EmberAfAttributeMetadata attributes_[]={
  {ZAP_EMPTY_DEFAULT(),0,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
  {ZAP_EMPTY_DEFAULT(),1,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
  {ZAP_EMPTY_DEFAULT(),2,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
  {ZAP_EMPTY_DEFAULT(),3,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
  {ZAP_EMPTY_DEFAULT(),0xFFFC,4,ZAP_TYPE(BITMAP32),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
  {ZAP_EMPTY_DEFAULT(),0xFFFD,2,ZAP_TYPE(INT16U),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)}};
 inline static EmberAfCluster clusters_[]={{chip::app::Clusters::Descriptor::Id,attributes_,MATTER_ARRAY_SIZE(attributes_),0,ZAP_CLUSTER_MASK(SERVER),nullptr,nullptr,nullptr}};
 inline static EmberAfEndpointType endpoint_={clusters_,MATTER_ARRAY_SIZE(clusters_),0};
 inline static constexpr EmberAfDeviceType types_[]={{0xFFF10001,1}};
 chip::DataVersion versions_[1]{};bool active_=false;
};
}
