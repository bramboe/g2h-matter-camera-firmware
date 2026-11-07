#pragma once
namespace g2h {
inline bool SnapshotDimensions(unsigned width,unsigned height){
    return (width==320&&height==180)||(width==640&&height==360);
}
inline bool SnapshotFits(unsigned width,unsigned height,unsigned minWidth,unsigned minHeight,unsigned maxWidth,unsigned maxHeight){
    return SnapshotDimensions(width,height)&&minWidth&&minHeight&&minWidth<=width&&minHeight<=height&&maxWidth>=width&&maxHeight>=height;
}
}
