from django.shortcuts import render
from django.http import HttpResponse, Http404

from lessons.models import Lesson, Example

def index(request):
    latest_lesson_list = Lesson.objects.all()[:5]
    context = ({
        'latest_lesson_list':latest_lesson_list,
    })
    return render(request, 'lessons/index.html', context)

def read(request, lesson_id):
    try:
        lesson = Lesson.objects.get(pk = lesson_id)
        example_list = Example.objects.filter(lesson__id = lesson_id)
        context = ({'lesson': lesson,'example_list':example_list,})
    except Lesson.DoesNotExist:
        raise Http404
    return render(request, 'lessons/read.html', context)

def question(request, lesson_id):
    return HttpResponse("You are looking at a question for lesson %s." % lesson_id)

